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
    // Save several pages (a chapter list) as one EPUB with a chapter table of contents.
    // Each URL is loaded in a hidden page and extracted like start() does.
    void startSeries(const QList<QUrl> &urls, const QString &seriesTitle);
    bool isRunning() const { return m_running; }

    // Chapters of the work this page belongs to, found by js/series.js (same-site links
    // under the page's folder). Empty when the page doesn't look like part of a series.
    struct ChapterLink { QUrl url; QString title; };
    static QList<ChapterLink> findChapters(QWebPage *page, QString *listTitle = 0);

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
    void chapterLoaded(bool ok);
    void chapterTimeout();

private:
    struct Image {
        QUrl url;
        QString name;        // images/imgN.jpg
        QByteArray data;
        QString mediaType;
        bool ok;
        Image() : ok(false) {}
    };
    struct Chapter { QString title; QString content; QUrl url; QList<Image> images; };
    bool extract(QWebPage *page);
    void collectImages();
    void fetchNext();
    void imagesDone();
    void storeImage(Image &img, const QByteArray &raw);
    void writeEpub();
    void fail(const QString &why);
    void loadNextChapter();

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
    // series mode
    bool m_series;
    QList<QUrl> m_urls;
    QString m_seriesTitle;
    QList<Chapter> m_chapters;
    int m_chapterIndex;
    QWebPage *m_hidden;
    QTimer m_chapterTimer;
};

// Overlay asking whether to save just this page or the whole series it belongs to.
class SaveChoiceDialog : public QWidget {
    Q_OBJECT
public:
    SaveChoiceDialog(int chapterCount, const QString &seriesTitle, QWidget *parent);
signals:
    void pageChosen();
    void seriesChosen();
    void cancelled();
protected:
    void resizeEvent(QResizeEvent *e);
    void mousePressEvent(QMouseEvent *e);
private:
    void place();
    QWidget *m_box;
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
