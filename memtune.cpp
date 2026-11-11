#include "memtune.h"
#include "adblock.h"
#include <QtWebKit/QWebSettings>
#include <QtGui/QPixmapCache>
#include <QtGui/QImage>
#include <QtGui/QImageReader>
#include <QtGui/QImageWriter>
#include <QtCore/QBuffer>
#include <QtCore/QStringList>
#include <QtCore/QElapsedTimer>
#include <QtCore/QDebug>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <malloc.h>

static int envInt(const char *name, int def)
{
    const char *v = getenv(name);
    return (v && *v) ? atoi(v) : def;
}

MemTune::MemTune(QObject *parent) : QObject(parent)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, SIGNAL(timeout()), this, SLOT(trimNow()));
}

void MemTune::applySettings(QWebSettings *ws)
{
    // WebCore's memory cache: decoded images, scripts, style sheets. Defaults are sized for
    // desktops; dead entries (from pages already left) are worthless here.
    int minDead = 0, maxDead = 2048, total = 6144;   // KB
    const char *oc = getenv("KB_OBJECT_CACHE");
    if (oc && *oc) {
        QStringList p = QString::fromLatin1(oc).split(',');
        if (p.size() == 3) { minDead = p[0].toInt(); maxDead = p[1].toInt(); total = p[2].toInt(); }
    }
    QWebSettings::setObjectCacheCapacities(minDead * 1024, maxDead * 1024, total * 1024);
    // Back/forward page cache keeps whole live pages (DOM, JS heap, images) in RAM.
    QWebSettings::setMaximumPagesInCache(envInt("KB_PAGES_IN_CACHE", 0));
    QPixmapCache::setCacheLimit(envInt("KB_PIXMAP_CACHE_KB", 2048));

    ws->setAttribute(QWebSettings::JavascriptEnabled, !getenv("KB_NO_JS"));
    ws->setAttribute(QWebSettings::JavaEnabled, false);
    ws->setAttribute(QWebSettings::PluginsEnabled, false);
    ws->setAttribute(QWebSettings::DnsPrefetchEnabled, false);
    ws->setAttribute(QWebSettings::OfflineStorageDatabaseEnabled, false);
    ws->setAttribute(QWebSettings::OfflineWebApplicationCacheEnabled, false);
    ws->setAttribute(QWebSettings::WebGLEnabled, false);
    ws->setAttribute(QWebSettings::WebAudioEnabled, false);
    ws->setAttribute(QWebSettings::NotificationsEnabled, false);
    ws->setAttribute(QWebSettings::HyperlinkAuditingEnabled, false);
    ws->setAttribute(QWebSettings::ScrollAnimatorEnabled, false);
    ws->setAttribute(QWebSettings::TiledBackingStoreEnabled, false);
    // Composited layers each get their own backing image; paint everything into one.
    if (!getenv("KB_ACCEL_COMPOSITING"))
        ws->setAttribute(QWebSettings::AcceleratedCompositingEnabled, false);
    // localStorage stays on (some sites break without it), but nothing persists to disk.
    qDebug() << "memtune: object cache KB" << minDead << maxDead << total
             << "pages in cache" << QWebSettings::maximumPagesInCache()
             << "js" << ws->testAttribute(QWebSettings::JavascriptEnabled);
}

qint64 MemTune::diskCacheBytes()
{
    return qint64(envInt("KB_DISK_CACHE_MB", 10)) * 1024 * 1024;
}

void MemTune::scheduleTrim()
{
    if (getenv("KB_NO_TRIM")) return;
    m_timer.start(envInt("KB_TRIM_DELAY_MS", 3000));
}

static long vmRssKb()
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[128];
    long kb = -1;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "VmRSS:", 6)) { kb = atol(line + 6); break; }
    fclose(f);
    return kb;
}

void MemTune::trimNow()
{
    QElapsedTimer t; t.start();
    long before = vmRssKb();
    // clearMemoryCaches() also drops the decoded images of the page being shown, so WebKit
    // decodes and repaints them again (measured: ~1.8x CPU, ~70% more screen updates).
    // The small object cache already limits dead entries; KB_TRIM_CLEAR=1 clears anyway.
    if (getenv("KB_TRIM_CLEAR")) QWebSettings::clearMemoryCaches();
    // DumpRenderTreeSupportQt::garbageCollectorCollect() is exported by the firmware's
    // libQtWebKit but has no public header; look it up so a missing symbol isn't fatal.
    typedef void (*GcFn)();
    static GcFn gc = (GcFn)dlsym(RTLD_DEFAULT, "_ZN23DumpRenderTreeSupportQt23garbageCollectorCollectEv");
    if (gc) gc();
    malloc_trim(0);
    qDebug() << "memtune: trim RSS" << before << "->" << vmRssKb() << "KB in" << t.elapsed() << "ms"
             << (gc ? "" : "(no JS GC hook)");
}

// ---------------------------------------------------------------------------

KoboNetworkAccessManager::KoboNetworkAccessManager(QObject *parent)
    : QNetworkAccessManager(parent), m_filter(!getenv("KB_NO_IMG_FILTER"))
{
}

static bool isImageRequest(const QNetworkRequest &req)
{
    if (req.rawHeader("Accept").startsWith("image/")) return true;
    QString path = req.url().path().toLower();
    return path.endsWith(".gif") || path.endsWith(".jpg") || path.endsWith(".jpeg") || path.endsWith(".png");
}

QNetworkReply *KoboNetworkAccessManager::createRequest(Operation op, const QNetworkRequest &req, QIODevice *outgoingData)
{
    if (AdBlock::instance()->shouldBlock(req.url()))
        return new BlockedReply(req, op, this);
    QNetworkReply *r = QNetworkAccessManager::createRequest(op, req, outgoingData);
    QString scheme = req.url().scheme();
    if (!m_filter || op != GetOperation || (scheme != "http" && scheme != "https") || !isImageRequest(req))
        return r;
    return new ImageFilterReply(r, envInt("KB_IMG_MAX_PX", 1600), this);
}

ImageFilterReply::ImageFilterReply(QNetworkReply *inner, int maxPx, QObject *parent)
    : QNetworkReply(parent), m_inner(inner), m_pos(0), m_maxPx(maxPx), m_done(false)
{
    m_inner->setParent(this);
    setRequest(inner->request());
    setUrl(inner->url());
    setOperation(inner->operation());
    QIODevice::open(QIODevice::ReadOnly);
    connect(m_inner, SIGNAL(finished()), this, SLOT(innerFinished()));
    connect(m_inner, SIGNAL(sslErrors(QList<QSslError>)), this, SIGNAL(sslErrors(QList<QSslError>)));
    if (m_inner->isFinished()) QMetaObject::invokeMethod(this, "innerFinished", Qt::QueuedConnection);
}

ImageFilterReply::~ImageFilterReply() {}

void ImageFilterReply::ignoreSslErrors()
{
    m_inner->ignoreSslErrors();
    QNetworkReply::ignoreSslErrors();
}

void ImageFilterReply::copyMeta()
{
    foreach (const QByteArray &h, m_inner->rawHeaderList())
        setRawHeader(h, m_inner->rawHeader(h));
    static const QNetworkRequest::Attribute attrs[] = {
        QNetworkRequest::HttpStatusCodeAttribute, QNetworkRequest::HttpReasonPhraseAttribute,
        QNetworkRequest::RedirectionTargetAttribute, QNetworkRequest::ConnectionEncryptedAttribute,
        QNetworkRequest::SourceIsFromCacheAttribute, QNetworkRequest::HttpPipeliningWasUsedAttribute };
    for (unsigned i = 0; i < sizeof attrs / sizeof attrs[0]; ++i) {
        QVariant v = m_inner->attribute(attrs[i]);
        if (v.isValid()) setAttribute(attrs[i], v);
    }
}

// Returns the rewritten image, or an empty array to keep the original.
static QByteArray filterImage(const QByteArray &in, int maxPx, QByteArray *mime, QString *why)
{
    QBuffer buf;
    buf.setData(in);
    buf.open(QIODevice::ReadOnly);
    QImageReader rd(&buf);
    QByteArray fmt = rd.format();
    if (fmt.isEmpty() || fmt == "svg") return QByteArray();
    QSize sz = rd.size();
    bool animated = fmt == "gif" && rd.supportsAnimation() && rd.imageCount() > 1;
    // Downscale only big photos: sprite sheets (CSS background-position) are usually
    // narrow in one dimension, and scaling them would break the page.
    bool big = sz.isValid() && qint64(sz.width()) * sz.height() > qint64(maxPx) * maxPx
               && qMin(sz.width(), sz.height()) >= 400 && maxPx > 0;
    if (!animated && !big) return QByteArray();
    if (big) rd.setScaledSize(sz.scaled(maxPx, maxPx, Qt::KeepAspectRatio));
    QImage img = rd.read();
    if (img.isNull()) return QByteArray();
    QByteArray out;
    QBuffer ob(&out);
    ob.open(QIODevice::WriteOnly);
    bool jpeg = fmt == "jpeg" || fmt == "jpg";
    if (!img.save(&ob, jpeg ? "JPEG" : "PNG", jpeg ? 85 : -1)) return QByteArray();
    *mime = jpeg ? "image/jpeg" : "image/png";
    *why = QString("%1%2 %3x%4 -> %5x%6").arg(animated ? "animated gif " : "").arg(QString(fmt))
               .arg(sz.width()).arg(sz.height()).arg(img.width()).arg(img.height());
    return out;
}

void ImageFilterReply::innerFinished()
{
    if (m_done) return;
    m_done = true;
    copyMeta();
    m_data = m_inner->readAll();
    if (m_inner->error() == QNetworkReply::NoError && !m_data.isEmpty()) {
        QElapsedTimer t; t.start();
        QByteArray mime;
        QString why;
        QByteArray out = filterImage(m_data, m_maxPx, &mime, &why);
        if (!out.isEmpty()) {
            qDebug() << "imgfilter:" << why << m_data.size() << "->" << out.size() << "bytes,"
                     << t.elapsed() << "ms" << url().toString().left(100);
            m_data = out;
            setHeader(QNetworkRequest::ContentTypeHeader, mime);
            setHeader(QNetworkRequest::ContentLengthHeader, m_data.size());
        }
    }
    emit metaDataChanged();
    if (m_inner->error() != QNetworkReply::NoError) {
        setError(m_inner->error(), m_inner->errorString());
        emit error(m_inner->error());
    }
    if (!m_data.isEmpty()) {
        emit downloadProgress(m_data.size(), m_data.size());
        emit readyRead();
    }
    setFinished(true);
    emit finished();
}

void ImageFilterReply::abort()
{
    if (m_done) return;
    m_done = true;
    disconnect(m_inner, 0, this, 0);
    m_inner->abort();
    setError(OperationCanceledError, "Operation canceled");
    emit error(OperationCanceledError);
    setFinished(true);
    emit finished();
}

qint64 ImageFilterReply::bytesAvailable() const
{
    return m_data.size() - m_pos + QNetworkReply::bytesAvailable();
}

qint64 ImageFilterReply::readData(char *data, qint64 maxSize)
{
    qint64 n = qMin(maxSize, qint64(m_data.size()) - m_pos);
    if (n <= 0) return m_done ? -1 : 0;
    memcpy(data, m_data.constData() + m_pos, n);
    m_pos += n;
    return n;
}
