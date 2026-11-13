// Downloads: a link to a book or document KOReader can open (PDF, EPUB, CBZ, DjVu, FB2,
// MOBI, ...) is saved to the library instead of doing nothing. WebKit hands over such
// responses through QWebPage::unsupportedContent (setForwardUnsupportedContent) and
// QWebPage::downloadRequested; the file streams to "<name>.part" in the library folder
// and is renamed when complete, so KOReader never sees half a book.
#ifndef KOBO_DOWNLOAD_H
#define KOBO_DOWNLOAD_H

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QFile;

class Downloader : public QObject {
    Q_OBJECT
public:
    Downloader(QNetworkAccessManager *nam, const QString &outDir, QObject *parent = 0);
    ~Downloader();
    bool isRunning() const { return m_reply != 0; }

    // The file extension KOReader would open this response as ("pdf", "epub", ...), or
    // an empty string. Looks at the file name (Content-Disposition, then the URL) first,
    // then the content type.
    static QString bookExtension(const QString &fileName, const QString &contentType);
    // File name from Content-Disposition (filename*= or filename=), else the URL's last
    // path segment; percent-decoded, without folders.
    static QString suggestedName(const QByteArray &contentDisposition, const QUrl &url);
    // A name in dir that doesn't exist yet: "name.ext", "name (2).ext", ...
    static QString uniquePath(const QString &dir, const QString &base, const QString &ext);

public slots:
    // Takes over a reply WebKit couldn't show. Returns false (and aborts the reply) when
    // it isn't a file KOReader opens; whyNot then says what it was.
    bool take(QNetworkReply *reply, QString *whyNot = 0);
    // Starts a download of a request (QWebPage::downloadRequested).
    void fetch(const QNetworkRequest &request);
    void cancel();

signals:
    void progress(const QString &message);
    void finished(bool ok, const QString &message);   // message = saved path or error

private slots:
    void metaData();
    void readMore();
    void replyFinished();
    void received(qint64 done, qint64 total);

private:
    void attach(QNetworkReply *reply);
    bool decide(QString *whyNot);
    void fail(const QString &why);
    void cleanup();

    QNetworkAccessManager *m_nam;
    QString m_outDir;
    QNetworkReply *m_reply;
    QFile *m_file;
    QString m_name;        // final file name, shown in messages
    QString m_finalPath;
    bool m_decided;
    bool m_fetched;        // our own request: follow redirects and decide on the headers
    int m_redirects;
    int m_lastShown;       // last percentage (or MB) shown, to keep e-ink updates few
};

#endif
