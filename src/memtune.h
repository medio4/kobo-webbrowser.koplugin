// Memory / CPU tuning for the Kobo's 512 MB RAM: small WebKit caches, a memory trim
// after each page load, and an image filter that downscales huge images and freezes
// animated GIFs on their first frame (e-ink can't show animation anyway).
#ifndef MEMTUNE_H
#define MEMTUNE_H
#include <QtCore/QObject>
#include <QtCore/QByteArray>
#include <QtCore/QTimer>
#include <QtCore/QPointer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include "csscompat.h"
class QWebSettings;

class MemTune : public QObject {
    Q_OBJECT
public:
    explicit MemTune(QObject *parent = 0);
    // Global WebKit caches + per-page attributes. Env overrides:
    //   KB_OBJECT_CACHE="minDead,maxDead,total" in KB (default 0,2048,6144)
    //   KB_PAGES_IN_CACHE (default 0: back/forward reloads from the disk cache)
    //   KB_PIXMAP_CACHE_KB (default 2048), KB_NO_JS=1 turns JavaScript off.
    static void applySettings(QWebSettings *ws);
    static qint64 diskCacheBytes();   // KB_DISK_CACHE_MB (default 10)
public slots:
    // Called on loadFinished: a few seconds later (after the e-ink refresh), run the
    // JavaScript GC and return freed heap to the system (KB_TRIM_CLEAR=1: also clear caches).
    // KB_NO_TRIM=1 disables.
    void scheduleTrim();
    void trimNow();
private:
    QTimer m_timer;
};

// Wraps image requests (Accept: image/... or *.gif) and rewrites the body when it
// is an animated GIF (-> PNG of the first frame) or larger than KB_IMG_MAX_PX
// (default 1600) on its longest side (-> scaled down, same format if writable).
// KB_NO_IMG_FILTER=1 disables. Requests to blocklisted hosts are dropped first (adblock.h).
// Style sheets and HTML pages go through CssCompatReply (csscompat.h): off with
// KB_NO_CSS_COMPAT=1 or a css-compat-off.txt beside the binary (KOReader menu, re-checked
// at each page navigation); KB_CSS_COMPAT_LOG=1 logs what each reply changed and how long it took.
class KoboNetworkAccessManager : public QNetworkAccessManager {
    Q_OBJECT
public:
    explicit KoboNetworkAccessManager(QObject *parent = 0);
protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &req, QIODevice *outgoingData);
private:
    bool m_filter;
    bool m_cssEnvOff, m_cssOff;
    CssCompat::Vars m_pageVars;   // custom properties of the page being loaded
};

// Passes a reply through, rewriting it with CssCompat when it turns out to be a style sheet
// (buffered: WebKit parses a sheet only once it is complete anyway) or an HTML page
// (streamed; only an unfinished tag or <style> element is held back).
class CssCompatReply : public QNetworkReply {
    Q_OBJECT
public:
    CssCompatReply(QNetworkReply *inner, CssCompat::Vars *pageVars, QObject *parent);
    ~CssCompatReply();
    void abort();
    qint64 bytesAvailable() const;
    bool isSequential() const { return true; }
public slots:
    void ignoreSslErrors();
protected:
    qint64 readData(char *data, qint64 maxSize);
private slots:
    void innerMetaData();
    void innerReadyRead();
    void innerFinished();
private:
    enum Mode { Undecided, Pass, Css, Html };
    void decide();
    void append(const QByteArray &d);
    QPointer<QNetworkReply> m_inner;
    CssCompat::Vars *m_vars;
    CssCompat::HtmlRewriter *m_html;
    Mode m_mode;
    QByteArray m_data, m_raw;
    qint64 m_pos;
    qint64 m_nsecs;
    bool m_done, m_log;
};

class ImageFilterReply : public QNetworkReply {
    Q_OBJECT
public:
    ImageFilterReply(QNetworkReply *inner, int maxPx, QObject *parent);
    ~ImageFilterReply();
    void abort();
    qint64 bytesAvailable() const;
    bool isSequential() const { return true; }
public slots:
    void ignoreSslErrors();
protected:
    qint64 readData(char *data, qint64 maxSize);
private slots:
    void innerFinished();
private:
    void copyMeta();
    QPointer<QNetworkReply> m_inner;
    QByteArray m_data;
    qint64 m_pos;
    int m_maxPx;
    bool m_done;
};
#endif
