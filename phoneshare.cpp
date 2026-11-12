#include "icons.h"
#include "phoneshare.h"
#include <QtCore/QDebug>
#include <QtCore/QUrl>
#include <QtNetwork/QTcpSocket>
#include <QtNetwork/QNetworkInterface>
#include <QtNetwork/QHostAddress>

PhoneShare::PhoneShare(QObject *parent) : QObject(parent), m_port(0)
{
    connect(&m_server, SIGNAL(newConnection()), this, SLOT(newConnection()));
}

bool PhoneShare::listen(quint16 port)
{
    if (!m_server.listen(QHostAddress::Any, port)) {
        qWarning() << "phone: cannot listen on port" << port << m_server.errorString();
        return false;
    }
    m_port = port;
    qDebug() << "phone: listening at" << address();
    return true;
}

QString PhoneShare::address() const
{
    if (!m_port) return QString();
    foreach (const QHostAddress &a, QNetworkInterface::allAddresses()) {
        if (a.protocol() != QAbstractSocket::IPv4Protocol || a == QHostAddress::LocalHost) continue;
        if (a.toString().startsWith("169.254.")) continue;
        return QString("http://%1:%2/").arg(a.toString()).arg(m_port);
    }
    return QString();
}

void PhoneShare::newConnection()
{
    while (QTcpSocket *s = m_server.nextPendingConnection()) {
        connect(s, SIGNAL(readyRead()), this, SLOT(readyRead()));
        connect(s, SIGNAL(disconnected()), s, SLOT(deleteLater()));
        m_buf[s] = QByteArray();
    }
}

void PhoneShare::readyRead()
{
    QTcpSocket *s = qobject_cast<QTcpSocket *>(sender());
    if (!s) return;
    QByteArray &buf = m_buf[s];
    buf += s->readAll();
    if (buf.size() > 65536) { s->close(); m_buf.remove(s); return; }
    int hdrEnd = buf.indexOf("\r\n\r\n");
    if (hdrEnd < 0) return;
    int len = 0;
    int cl = buf.toLower().indexOf("content-length:");
    if (cl >= 0 && cl < hdrEnd) len = buf.mid(cl + 15, buf.indexOf("\r\n", cl) - cl - 15).trimmed().toInt();
    if (buf.size() < hdrEnd + 4 + len) return;           // body still coming
    QByteArray req = buf;
    m_buf.remove(s);
    handle(s, req);
}

QMap<QString, QString> PhoneShare::params(const QByteArray &query)
{
    QMap<QString, QString> out;
    foreach (const QByteArray &kv, query.split('&')) {
        int eq = kv.indexOf('=');
        QByteArray k = eq < 0 ? kv : kv.left(eq), v = eq < 0 ? QByteArray() : kv.mid(eq + 1);
        v.replace('+', ' ');
        out[QString::fromUtf8(QByteArray::fromPercentEncoding(k))] = QString::fromUtf8(QByteArray::fromPercentEncoding(v));
    }
    return out;
}

static QByteArray esc(const QString &s)
{
    QString t = s;
    t.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;");
    return t.toUtf8();
}

void PhoneShare::handle(QTcpSocket *s, const QByteArray &request)
{
    int lineEnd = request.indexOf("\r\n");
    QList<QByteArray> parts = request.left(lineEnd).split(' ');
    QByteArray method = parts.value(0), target = parts.value(1);
    int hdrEnd = request.indexOf("\r\n\r\n");
    QByteArray body = request.mid(hdrEnd + 4);
    QByteArray path = target, query;
    int q = target.indexOf('?');
    if (q >= 0) { path = target.left(q); query = target.mid(q + 1); }

    QByteArray status = "200 OK", type = "text/html; charset=utf-8", out;
    if (path == "/" || path == "/open") {
        QString notice;
        QMap<QString, QString> p = params(method == "POST" ? body : query);
        QString url = p.value("url").trimmed();
        // Shared text from a phone often carries the title before the link: keep the link.
        int h = url.indexOf(QRegExp("https?://", Qt::CaseInsensitive));
        if (h >= 0) url = url.mid(h);
        url = url.section(QRegExp("\\s"), 0, 0);
        if (!url.isEmpty()) {
            if (!url.contains("://")) url = "http://" + url;
            QUrl u = QUrl::fromUserInput(url);
            if (u.isValid() && (u.scheme() == "http" || u.scheme() == "https")) {
                m_recent.prepend(u.toString());
                while (m_recent.size() > 8) m_recent.removeLast();
                emit urlReceived(u.toString());
                notice = "Opening on the Kobo: " + u.toString();
            } else {
                notice = "That does not look like a web address.";
            }
        }
        out = page(notice);
    } else if (path == "/current") {
        type = "text/plain; charset=utf-8";
        out = m_pageUrl.toUtf8();
    } else {
        status = "404 Not Found"; type = "text/plain"; out = "not found";
    }
    QByteArray resp = "HTTP/1.1 " + status + "\r\nContent-Type: " + type +
        "\r\nContent-Length: " + QByteArray::number(out.size()) + "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n" + out;
    s->write(resp);
    s->disconnectFromHost();
}

QByteArray PhoneShare::page(const QString &notice) const
{
    QByteArray h =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Send to Kobo</title>"
        "<style>body{font:18px/1.4 -apple-system,Helvetica,Arial,sans-serif;margin:0;padding:20px;background:#f4f4f4;color:#111}"
        "h1{font-size:22px;margin:0 0 14px}form{background:#fff;border-radius:12px;padding:16px;box-shadow:0 1px 3px rgba(0,0,0,.15)}"
        "input[name=url]{width:100%;box-sizing:border-box;font-size:18px;padding:12px;border:1px solid #bbb;border-radius:8px}"
        "button{width:100%;margin-top:12px;font-size:19px;padding:14px;border:0;border-radius:8px;background:#222;color:#fff}"
        ".n{background:#dff0d8;border-radius:8px;padding:12px;margin-bottom:14px;word-break:break-all}"
        ".c{color:#555;font-size:15px;margin-top:18px;word-break:break-all}ul{padding-left:18px}li{margin:6px 0;word-break:break-all}"
        "a{color:#124}</style></head><body><h1>Send to Kobo</h1>";
    if (!notice.isEmpty()) h += "<div class=\"n\">" + esc(notice) + "</div>";
    h += "<form method=\"post\" action=\"/open\">"
         "<input type=\"text\" inputmode=\"url\" name=\"url\" placeholder=\"Paste a link or type an address\" autofocus autocomplete=\"off\" autocapitalize=\"off\" autocorrect=\"off\" spellcheck=\"false\">"
         "<button type=\"submit\">Open on the Kobo</button></form>";
    if (!m_pageUrl.isEmpty())
        h += "<div class=\"c\">Kobo is showing: <a href=\"" + esc(m_pageUrl) + "\">" + esc(m_pageTitle.isEmpty() ? m_pageUrl : m_pageTitle) + "</a></div>";
    if (!m_recent.isEmpty()) {
        h += "<div class=\"c\">Sent recently:<ul>";
        foreach (const QString &r, m_recent)
            h += "<li><a href=\"/open?url=" + QUrl::toPercentEncoding(r) + "\">" + esc(r) + "</a></li>";
        h += "</ul></div>";
    }
    h += "<div class=\"c\">Tip: bookmark this page on your phone. Sharing a link from another app to this page "
         "works by pasting it here.</div></body></html>";
    return h;
}

// ------------------------------------------------------------- PhoneDialog

#include "third_party/qrcodegen.h"
#include <QtGui/QImage>
#include <QtGui/QPixmap>
#include <QtGui/QResizeEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QFrame>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

static QImage qrImage(const QString &text, int pixels)
{
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    QByteArray t = text.toUtf8();
    if (!qrcodegen_encodeText(t.constData(), tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                              qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
        return QImage();
    int n = qrcodegen_getSize(qr);
    const int quiet = 4;
    int scale = qMax(1, pixels / (n + 2 * quiet));
    int side = (n + 2 * quiet) * scale;
    QImage img(side, side, QImage::Format_RGB32);
    img.fill(Qt::white);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            if (qrcodegen_getModule(qr, x, y))
                for (int dy = 0; dy < scale; ++dy)
                    for (int dx = 0; dx < scale; ++dx)
                        img.setPixel((x + quiet) * scale + dx, (y + quiet) * scale + dy, 0xff000000);
    return img;
}

PhoneDialog::PhoneDialog(const QString &url, QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setProperty("kb_overlay", true);   // e-ink: flash its area clean when it closes
    setAutoFillBackground(false);
    QFrame *box = new QFrame(this);
    m_box = box;
    box->setFrameStyle(QFrame::Box | QFrame::Plain);
    box->setLineWidth(3);
    box->setAutoFillBackground(true);
    QPalette pal = box->palette();
    pal.setColor(QPalette::Window, Qt::white);
    pal.setColor(QPalette::WindowText, Qt::black);
    box->setPalette(pal);

    QVBoxLayout *v = new QVBoxLayout(box);
    v->setContentsMargins(30, 30, 30, 30);
    v->setSpacing(20);
    QFont f; f.setPixelSize(38);
    QFont fs; fs.setPixelSize(28);

    QLabel *head = new QLabel("Send to Kobo from your phone");
    head->setFont(f); head->setAlignment(Qt::AlignCenter); head->setWordWrap(true);
    v->addWidget(head);

    QLabel *pic = new QLabel;
    pic->setAlignment(Qt::AlignCenter);
    QImage img = url.isEmpty() ? QImage() : qrImage(url, 620);
    if (!img.isNull()) pic->setPixmap(QPixmap::fromImage(img));
    else { pic->setText(url.isEmpty() ? "Not connected to Wi-Fi" : "QR code unavailable"); pic->setFont(fs); }
    v->addWidget(pic);

    QLabel *txt = new QLabel(url.isEmpty()
        ? QString("Connect the Kobo to Wi-Fi, then come back here.")
        : "Scan with the phone's camera, or type " + url + " into its browser. Paste a link there and it opens here. "
          "Works while this browser is open.");
    txt->setFont(fs); txt->setWordWrap(true); txt->setAlignment(Qt::AlignCenter);
    v->addWidget(txt);

    QPushButton *close = new QPushButton("Close");
    close->setFont(f); close->setMinimumSize(300, 100); close->setFocusPolicy(Qt::NoFocus);
    close->setProperty("kb_fast", true);
    KbIcons::setOn(close, "close");
    v->addWidget(close);
    connect(close, SIGNAL(clicked()), this, SIGNAL(dismissed()));
    connect(close, SIGNAL(clicked()), this, SLOT(close()));

    if (parent) resize(parent->size());
    place();
    raise();
    show();
}

void PhoneDialog::place()
{
    int w = qMin(width() - 40, 900);
    m_box->setFixedWidth(w);
    m_box->adjustSize();
    m_box->move((width() - m_box->width()) / 2, (height() - m_box->height()) / 2);
}

void PhoneDialog::resizeEvent(QResizeEvent *e) { QWidget::resizeEvent(e); place(); }
void PhoneDialog::mousePressEvent(QMouseEvent *e) { e->accept(); }
