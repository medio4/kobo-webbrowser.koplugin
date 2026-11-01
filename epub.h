// Save the current page as an EPUB in the KOReader library: article extraction with
// Mozilla's Readability.js (run inside the page), images fetched and downscaled,
// written as a stored (uncompressed) zip.
#ifndef EPUB_H
#define EPUB_H

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QByteArray>
#include <QtCore/QUrl>
#include <QtCore/QList>
#include <QtCore/QTimer>

class QWebPage;
class QNetworkReply;

class EpubSaver : public QObject {
    Q_OBJECT
public:
    // outDir: folder for the .epub (created if missing).
    EpubSaver(QWebPage *page, const QString &outDir, QObject *parent = 0);
    ~EpubSaver();
    void start();
    bool isRunning() const { return m_running; }

    // Folder for saved pages: $KB_LIBRARY_DIR, else <KOReader home>/Web.
    static QString defaultOutDir();
    static QString safeFileName(const QString &title);

signals:
    void progress(const QString &message);
    void finished(bool ok, const QString &message);   // message = path or error

private slots:
    void imageFinished();
    void imageTimeout();

private:
    struct Image {
        QUrl url;
        QString name;        // images/imgN.jpg
        QByteArray data;
        QString mediaType;
        bool ok;
        Image() : ok(false) {}
    };
    bool extract();
    void collectImages();
    void fetchNext();
    void storeImage(Image &img, const QByteArray &raw);
    void writeEpub();
    void fail(const QString &why);

    QWebPage *m_page;
    QString m_outDir;
    bool m_running;
    QString m_title, m_byline, m_siteName, m_lang, m_excerpt, m_content;
    QUrl m_pageUrl;
    bool m_fallback;
    QList<Image> m_images;
    int m_current;
    QNetworkReply *m_reply;
    QTimer m_timer;
};

#endif
