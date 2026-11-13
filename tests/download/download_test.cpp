// Desktop test for src/download.cpp: file-name and type rules, then real downloads from a
// tiny HTTP server in this process, both through QWebPage::unsupportedContent (a tapped
// link) and through Downloader::fetch (with a redirect). Run with tests/download/run.sh.
#include "../../src/download.h"
#include <QtWidgets/QApplication>
#include <QtWebKitWidgets/QWebPage>
#include <QtWebKitWidgets/QWebFrame>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QDebug>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { QString x_ = (a), y_ = (b); if (x_ != y_) { ++failures; \
    printf("FAIL %s:%d: %s\n  got:  %s\n  want: %s\n", __FILE__, __LINE__, #a, qPrintable(x_), qPrintable(y_)); } } while (0)

static QByteArray bigBody() { QByteArray b("%PDF-1.4\n"); while (b.size() < 300000) b += "0123456789abcdef\n"; return b; }

class Server : public QTcpServer {
    Q_OBJECT
public:
    Server() { connect(this, SIGNAL(newConnection()), this, SLOT(accepted())); listen(QHostAddress::LocalHost); }
    QString base() const { return QString("http://127.0.0.1:%1").arg(serverPort()); }
private slots:
    void accepted() { while (QTcpSocket *s = nextPendingConnection()) connect(s, SIGNAL(readyRead()), this, SLOT(request())); }
    void request()
    {
        QTcpSocket *s = qobject_cast<QTcpSocket *>(sender());
        QByteArray req = s->property("buf").toByteArray() + s->readAll();
        if (!req.contains("\r\n\r\n")) { s->setProperty("buf", req); return; }
        QByteArray path = req.split(' ').value(1);
        QByteArray status = "200 OK", type = "text/html", extra, body;
        if (path == "/book.pdf") { type = "application/pdf"; body = bigBody(); }
        else if (path == "/get?id=7") { type = "application/octet-stream";
            extra = "Content-Disposition: attachment; filename=\"My Comic: part 1.cbz\"\r\n"; body = "PK\x03\x04 comic"; }
        else if (path == "/star") { type = "application/epub+zip";
            extra = "Content-Disposition: attachment; filename*=UTF-8''Caf%C3%A9%20book.epub\r\n"; body = "PK\x03\x04 epub"; }
        else if (path == "/blob.bin") { type = "application/octet-stream"; body = "binary"; }
        else if (path == "/moved") { status = "302 Found"; extra = "Location: /book.pdf\r\n"; }
        else if (path == "/missing.pdf") { status = "404 Not Found"; body = "nope"; }
        else body = "<html><body><a href=\"/book.pdf\">book</a></body></html>";
        s->write("HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\n" + extra +
                 "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        s->disconnectFromHost();
    }
};

// Waits for Downloader::finished (or 10 s).
class Waiter : public QObject {
    Q_OBJECT
public:
    bool done, ok; QString msg, why; Downloader *dl;
    Waiter(Downloader *d) : done(false), ok(false), dl(d) { connect(d, SIGNAL(finished(bool,QString)), this, SLOT(fin(bool,QString))); }
    void wait() { QEventLoop l; QTimer::singleShot(10000, &l, SLOT(quit())); connect(this, SIGNAL(stop()), &l, SLOT(quit())); if (!done) l.exec(); }
    void reset() { done = ok = false; msg.clear(); why.clear(); }
signals:
    void stop();
public slots:
    void fin(bool o, const QString &m) { done = true; ok = o; msg = m; emit stop(); }
    void unsupported(QNetworkReply *r) { if (!dl->take(r, &why)) fin(false, why); }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    // --- names and types
    CHECK_EQ(Downloader::bookExtension("a.PDF", ""), "pdf");
    CHECK_EQ(Downloader::bookExtension("a.kepub.epub", "application/octet-stream"), "epub");
    CHECK_EQ(Downloader::bookExtension("story.fb2.zip", "application/zip"), "fb2.zip");
    CHECK_EQ(Downloader::bookExtension("download", "application/pdf; charset=binary"), "pdf");
    CHECK_EQ(Downloader::bookExtension("view.php", "application/epub+zip"), "epub");
    CHECK_EQ(Downloader::bookExtension("archive.zip", "application/zip"), "");
    CHECK_EQ(Downloader::bookExtension("setup.exe", "application/octet-stream"), "");
    CHECK_EQ(Downloader::bookExtension("notes.txt", "application/octet-stream"), "txt");
    CHECK_EQ(Downloader::suggestedName("attachment; filename=\"A B.pdf\"", QUrl("http://x/y")), "A B.pdf");
    CHECK_EQ(Downloader::suggestedName("attachment; filename=plain.epub; size=3", QUrl("http://x/y")), "plain.epub");
    CHECK_EQ(Downloader::suggestedName("attachment; filename*=UTF-8''Caf%C3%A9.pdf", QUrl("http://x/y")), QString::fromUtf8("Caf\xc3\xa9.pdf"));
    CHECK_EQ(Downloader::suggestedName("attachment; filename=\"../../etc/passwd\"", QUrl("http://x/y")), "passwd");
    CHECK_EQ(Downloader::suggestedName("", QUrl("http://x/dir/My%20Book.pdf?dl=1")), "My Book.pdf");

    QString out = QDir::tempPath() + "/kb-download-test";
    QDir(out).removeRecursively();
    Server server;
    QWebPage page;
    page.setForwardUnsupportedContent(true);
    Downloader dl(page.networkAccessManager(), out);
    Waiter w(&dl);
    QObject::connect(&page, SIGNAL(unsupportedContent(QNetworkReply*)), &w, SLOT(unsupported(QNetworkReply*)));

    // --- a tapped PDF link, twice: the second gets its own name
    page.mainFrame()->load(QUrl(server.base() + "/book.pdf"));
    w.wait();
    CHECK(w.ok); CHECK_EQ(w.msg, out + "/book.pdf");
    CHECK(QFile(out + "/book.pdf").size() == bigBody().size());
    w.reset();
    page.mainFrame()->load(QUrl(server.base() + "/book.pdf"));
    w.wait();
    CHECK(w.ok); CHECK_EQ(w.msg, out + "/book (2).pdf");

    // --- a name from Content-Disposition, made safe for the Kobo's FAT file system
    w.reset();
    page.mainFrame()->load(QUrl(server.base() + "/get?id=7"));
    w.wait();
    CHECK(w.ok); CHECK_EQ(w.msg, out + "/My Comic- part 1.cbz");
    w.reset();
    page.mainFrame()->load(QUrl(server.base() + "/star"));
    w.wait();
    CHECK(w.ok); CHECK_EQ(w.msg, out + QString::fromUtf8("/Caf\xc3\xa9 book.epub"));

    // --- not a book: refused, nothing written
    w.reset();
    page.mainFrame()->load(QUrl(server.base() + "/blob.bin"));
    w.wait();
    CHECK(!w.ok); CHECK(w.msg.contains("KOReader can't open"));
    CHECK(!dl.isRunning());

    // --- fetch(): follows a redirect; reports an HTTP error
    w.reset();
    dl.fetch(QNetworkRequest(QUrl(server.base() + "/moved")));
    w.wait();
    CHECK(w.ok); CHECK_EQ(w.msg, out + "/book (3).pdf");
    w.reset();
    dl.fetch(QNetworkRequest(QUrl(server.base() + "/missing.pdf")));
    w.wait();
    CHECK(!w.ok); CHECK(w.msg.contains("404") || w.msg.contains("Not Found"));

    CHECK_EQ(QDir(out).entryList(QStringList() << "*.part").join(","), "");
    CHECK(QDir(out).entryList(QDir::Files).size() == 5);
    QDir(out).removeRecursively();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
#include "download_test.moc"
