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
class QFile;

class EpubSaver : public QObject {
    Q_OBJECT
public:
    // outDir: folder for the .epub (created if missing).
    EpubSaver(QWebPage *page, const QString &outDir, QObject *parent = 0);
    ~EpubSaver();
    void start();
    // Save several pages (a chapter list) as one EPUB with a chapter table of contents.
    // Each URL is loaded in a hidden page and extracted like start() does.
    struct ChapterLink { QUrl url; QString title; };
    void startSeries(const QList<ChapterLink> &links, const QString &seriesTitle);
    bool isRunning() const { return m_running; }
    // false: a temporary copy (reader mode), no "saved" handoff line, so it never joins
    // the KOReader collection.
    void setLibrary(bool library) { m_library = library; }
    // Comic mode: the page's images in order, written as a CBZ (one image per page)
    // instead of an EPUB. For a series, also switched on by itself when the first chapter
    // turns out to be just images.
    void setComic(bool comic) { m_comic = comic; }
    // A page that is mostly big images (manga, comics) rather than text.
    static bool looksLikeComic(QWebPage *page);
    // A chapter page loaded without pictures (series mode, hidden page): many pictures
    // and little text.
    static bool isComicChapter(QWebPage *page);
    // Series of comics: one CBZ per part instead of one for all of them.
    void setComicSplit(bool split) { m_comicSplit = split; if (split) m_comic = true; }
    QStringList savedFiles() const { return m_savedFiles; }

    // Chapters of the work this page belongs to, found by js/series.js (same-site links
    // under the page's folder). Empty when the page doesn't look like part of a series.
    static QList<ChapterLink> findChapters(QWebPage *page, QString *listTitle = 0);

    // Folder for saved pages: $KB_LIBRARY_DIR, else <KOReader home>/Web.
    static QString defaultOutDir();
    // Folder for reader-mode copies: $KB_READER_DIR, else reader-tmp next to the program.
    // Holds one book at a time; clearReaderDir() empties it before each new one.
    static QString readerDir();
    static void clearReaderDir();
    static QString safeFileName(const QString &title);

    // Handoff to KOReader (read by kobo-webbrowser.koplugin after the browser exits):
    // $KB_HANDOFF_FILE, lines "saved\t<path>" for every EPUB written, "open\t<path>" to open one,
    // "read\t<path>\t<url>\t<scrollY>\t<zoom>" to read a reader-mode copy and come back here.
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
    bool extractComic(QWebPage *page, bool hiddenPage);
    void addComicPage(const QByteArray &raw);
    bool writeComicImage(const QByteArray &jpeg);
    void finishComic();
    bool closeComicFile();
    void fail(const QString &why);
    void loadNextChapter();

    QWebPage *m_page;
    QString m_outDir;
    bool m_running;
    bool m_library;
    QString m_title, m_byline, m_siteName, m_lang, m_excerpt, m_content;
    QUrl m_pageUrl;
    bool m_fallback;
    QList<Image> m_images;
    int m_current;
    QNetworkReply *m_reply;
    QTimer m_timer;
    // series mode
    bool m_series;
    QList<ChapterLink> m_links;
    QString m_seriesTitle;
    QList<Chapter> m_chapters;
    int m_chapterIndex;
    QWebPage *m_hidden;
    QTimer m_chapterTimer;
    // comic mode: pages are streamed into the .cbz as they arrive (low memory)
    bool m_comic;
    bool m_comicDecided;
    struct CbzEntry { QByteArray name; quint32 crc; quint32 size; quint32 offset; };
    QFile *m_cbz;
    QString m_cbzPath;
    QList<CbzEntry> m_cbzEntries;
    bool m_comicSplit;
    QStringList m_savedFiles;
};

// Overlay asking whether to save just this page or the whole series it belongs to.
class SaveChoiceDialog : public QWidget {
    Q_OBJECT
public:
    // reading: worded for "Read in KOReader" (a temporary book) instead of saving.
    // comic: the parts are comics; also offers each part as its own file.
    SaveChoiceDialog(int chapterCount, const QString &seriesTitle, QWidget *parent, bool reading = false,
                     bool comic = false);
signals:
    void pageChosen();
    void seriesChosen();
    void seriesSplitChosen();
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
    // count > 1: several files were saved (epubPath is the first).
    SavedDialog(const QString &epubPath, QWidget *parent, int count = 1);
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
