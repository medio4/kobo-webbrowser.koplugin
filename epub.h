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
#include <QtWidgets/QWidget>

class QWebPage;
class QResizeEvent;
class QMouseEvent;
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

    // Handoff to KOReader (read by applauncher.koplugin after the browser exits):
    // $KB_HANDOFF_FILE, lines "saved\t<path>" for every EPUB written, "open\t<path>" to open one.
    static QString handoffFile();
    static bool appendHandoff(const QString &kind, const QString &path);

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


// In-window overlay shown after a save: "Open in KOReader now?" with big touch buttons.
// (A separate top-level window would not reach the e-ink grab path in eink.cpp.)
class SavedDialog : public QWidget {
    Q_OBJECT
public:
    SavedDialog(const QString &epubPath, QWidget *parent);
signals:
    void openRequested(const QString &path);
    void dismissed();
private slots:
    void openClicked();
    void laterClicked();
protected:
    void resizeEvent(QResizeEvent *e);
    void mousePressEvent(QMouseEvent *e);
private:
    void place();
    QString m_path;
    QWidget *m_box;
};

#endif
