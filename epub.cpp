#include "epub.h"

#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QBuffer>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QVariant>
#include <QtCore/QRegExp>
#include <QtCore/QtEndian>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtWebKit/QWebSettings>
#include <QtCore/QUuid>
#include <QtGui/QImage>
#include <QtGui/QImageReader>
#include <QtGui/QResizeEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QFont>
#include <QtWidgets/QFrame>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtCore/QCoreApplication>
#include <QtCore/QTextStream>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QtWebKitWidgets/QWebPage>
#include <QtWebKitWidgets/QWebFrame>

#include <cstdlib>
#include <unistd.h>

namespace {

const int kMaxImages = 60;
const int kMaxImageBytes = 8 * 1024 * 1024;
const int kMaxImageWidth = 1200;      // Kobo Clara BW screen is 1072 px wide
const int kImageTimeoutMs = 25000;
const int kMaxChapters = 300;
const int kChapterTimeoutMs = 60000;

// ------------------------------------------------------------- stored zip

quint32 crcTable[256];
bool crcInit = false;
quint32 crc32(const QByteArray &d)
{
    if (!crcInit) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            crcTable[i] = c;
        }
        crcInit = true;
    }
    quint32 c = 0xFFFFFFFFu;
    for (int i = 0; i < d.size(); ++i) c = crcTable[(c ^ (unsigned char)d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

struct Entry { QByteArray name; QByteArray data; quint32 crc; quint32 offset; };

void put16(QByteArray &b, quint16 v) { quint16 le = qToLittleEndian(v); b.append((const char *)&le, 2); }
void put32(QByteArray &b, quint32 v) { quint32 le = qToLittleEndian(v); b.append((const char *)&le, 4); }

QByteArray buildZip(QList<Entry> &entries)
{
    QByteArray out;
    const quint16 dosTime = 0, dosDate = (1 << 5) | 1; // 1980-01-01
    for (int i = 0; i < entries.size(); ++i) {
        Entry &e = entries[i];
        e.crc = crc32(e.data);
        e.offset = out.size();
        put32(out, 0x04034b50); put16(out, 20); put16(out, 0); put16(out, 0); // version, flags, stored
        put16(out, dosTime); put16(out, dosDate);
        put32(out, e.crc); put32(out, e.data.size()); put32(out, e.data.size());
        put16(out, e.name.size()); put16(out, 0);
        out.append(e.name); out.append(e.data);
    }
    quint32 cdStart = out.size();
    for (int i = 0; i < entries.size(); ++i) {
        const Entry &e = entries[i];
        put32(out, 0x02014b50); put16(out, 20); put16(out, 20); put16(out, 0); put16(out, 0);
        put16(out, dosTime); put16(out, dosDate);
        put32(out, e.crc); put32(out, e.data.size()); put32(out, e.data.size());
        put16(out, e.name.size()); put16(out, 0); put16(out, 0); put16(out, 0); put16(out, 0);
        put32(out, 0); put32(out, e.offset);
        out.append(e.name);
    }
    quint32 cdSize = out.size() - cdStart;
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0);
    put16(out, entries.size()); put16(out, entries.size());
    put32(out, cdSize); put32(out, cdStart); put16(out, 0);
    return out;
}

// ------------------------------------------------------------- helpers

QString xmlEscape(QString s)
{
    return s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;");
}

QString xmlUnescape(QString s)
{
    return s.replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"").replace("&apos;", "'")
            .replace("&#39;", "'").replace("&amp;", "&");
}

QString readResource(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { qWarning() << "epub: missing resource" << path; return QString(); }
    return QString::fromUtf8(f.readAll());
}

// Attribute value from a single tag's text, or empty.
QString tagAttr(const QString &tag, const QString &name)
{
    QRegExp re("\\s" + name + "\\s*=\\s*\"([^\"]*)\"", Qt::CaseInsensitive);
    if (re.indexIn(tag) >= 0) return xmlUnescape(re.cap(1));
    QRegExp re2("\\s" + name + "\\s*=\\s*'([^']*)'", Qt::CaseInsensitive);
    if (re2.indexIn(tag) >= 0) return xmlUnescape(re2.cap(1));
    return QString();
}

} // namespace

// ------------------------------------------------------------- EpubSaver

EpubSaver::EpubSaver(QWebPage *page, const QString &outDir, QObject *parent)
    : QObject(parent), m_page(page), m_outDir(outDir), m_running(false), m_library(true), m_fallback(false),
      m_current(-1), m_reply(0), m_series(false), m_chapterIndex(-1), m_hidden(0)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, SIGNAL(timeout()), this, SLOT(imageTimeout()));
    m_chapterTimer.setSingleShot(true);
    connect(&m_chapterTimer, SIGNAL(timeout()), this, SLOT(chapterTimeout()));
}

EpubSaver::~EpubSaver()
{
    if (m_reply) { m_reply->abort(); m_reply->deleteLater(); }
    if (m_hidden) { m_hidden->triggerAction(QWebPage::Stop); m_hidden->deleteLater(); }
}

// ------------------------------------------------------------- series

QList<EpubSaver::ChapterLink> EpubSaver::findChapters(QWebPage *page, QString *listTitle)
{
    QList<ChapterLink> out;
    QVariant v = page->mainFrame()->evaluateJavaScript(readResource(":/js/js/series.js"));
    QJsonDocument doc = QJsonDocument::fromJson(v.toString().toUtf8());
    if (!doc.isObject()) return out;
    QJsonObject o = doc.object();
    if (listTitle) *listTitle = o.value("title").toString().simplified();
    QJsonArray arr = o.value("chapters").toArray();
    for (int i = 0; i < arr.size() && i < kMaxChapters; ++i) {
        QJsonObject ch = arr.at(i).toObject();
        ChapterLink l;
        l.url = QUrl(ch.value("url").toString());
        l.title = ch.value("title").toString().simplified();
        if (l.url.isValid()) out << l;
    }
    return out;
}

void EpubSaver::startSeries(const QList<ChapterLink> &links, const QString &seriesTitle)
{
    if (m_running || links.isEmpty()) return;
    m_running = true;
    m_series = true;
    m_links = links;
    m_seriesTitle = seriesTitle.simplified();
    m_chapters.clear();
    m_chapterIndex = -1;
    if (!m_hidden) {
        // Off-screen page sharing the browser's network stack (cookies, cache). Images are
        // not decoded here; the saver fetches the ones the article keeps.
        m_hidden = new QWebPage(this);
        m_hidden->setNetworkAccessManager(m_page->networkAccessManager());
        m_hidden->settings()->setAttribute(QWebSettings::AutoLoadImages, false);
        m_hidden->settings()->setAttribute(QWebSettings::JavascriptEnabled, true);
        m_hidden->settings()->setAttribute(QWebSettings::PluginsEnabled, false);
        m_hidden->setViewportSize(QSize(1000, 1400));
        connect(m_hidden, SIGNAL(loadFinished(bool)), this, SLOT(chapterLoaded(bool)));
    }
    loadNextChapter();
}

void EpubSaver::loadNextChapter()
{
    ++m_chapterIndex;
    if (m_chapterIndex >= m_links.size()) {
        if (m_chapters.isEmpty()) { fail("None of the chapters could be read"); return; }
        writeEpub();
        return;
    }
    emit progress(QString("Saving series: chapter %1 of %2...").arg(m_chapterIndex + 1).arg(m_links.size()));
    m_chapterTimer.start(kChapterTimeoutMs);
    m_hidden->mainFrame()->load(m_links[m_chapterIndex].url);
}

void EpubSaver::chapterLoaded(bool ok)
{
    if (!m_running || !m_series) return;
    m_chapterTimer.stop();
    if (!ok) {
        qWarning() << "epub: chapter failed to load" << m_links[m_chapterIndex].url;
        loadNextChapter();
        return;
    }
    m_images.clear();
    m_title.clear(); m_content.clear(); m_byline.clear();
    blockSignals(true);               // extract() reports failure via fail(); a bad chapter is skipped
    bool got = extract(m_hidden);
    blockSignals(false);
    if (!got) {
        m_running = true;
        loadNextChapter();
        return;
    }
    collectImages();
    m_current = -1;
    fetchNext();
}

void EpubSaver::chapterTimeout()
{
    if (!m_running || !m_series) return;
    qWarning() << "epub: chapter timed out" << m_links[m_chapterIndex].url;
    m_hidden->triggerAction(QWebPage::Stop);
    loadNextChapter();
}

QString EpubSaver::defaultOutDir()
{
    const char *env = getenv("KB_LIBRARY_DIR");
    if (env && *env) return QString::fromLocal8Bit(env);
    if (QDir("/mnt/onboard/Books").exists()) return "/mnt/onboard/Books/Web";
    if (QDir("/mnt/onboard").exists()) return "/mnt/onboard/Web";
    return QDir::homePath() + "/kobo-browser-saved";
}

QString EpubSaver::readerDir()
{
    const char *env = getenv("KB_READER_DIR");
    if (env && *env) return QString::fromLocal8Bit(env);
    return QDir(QCoreApplication::applicationDirPath()).filePath("reader-tmp");
}

void EpubSaver::clearReaderDir()
{
    QDir dir(readerDir());
    if (!dir.exists()) return;
    foreach (const QString &name, dir.entryList(QStringList() << "*.epub", QDir::Files))
        dir.remove(name);
}

QString EpubSaver::safeFileName(const QString &title)
{
    QString t = title.simplified();
    t.replace(QRegExp("[\\\\/:*?\"<>|\\x00-\\x1f]"), "-");
    t.replace(QRegExp("^[.\\s-]+"), "");
    if (t.size() > 80) t = t.left(80).trimmed();
    if (t.isEmpty()) t = "Saved page";
    return t;
}

void EpubSaver::start()
{
    if (m_running) return;
    m_running = true;
    emit progress("Saving: extracting article...");
    m_series = false;
    if (!extract(m_page)) return;
    collectImages();
    m_current = -1;
    fetchNext();
}

void EpubSaver::fail(const QString &why)
{
    qWarning() << "epub:" << why;
    m_running = false;
    emit finished(false, why);
}

bool EpubSaver::extract(QWebPage *page)
{
    QWebFrame *frame = page->mainFrame();
    m_pageUrl = frame->url();
    QString script = "(function(){\n" + readResource(":/js/js/prelude.js") + "\n" +
                     readResource(":/js/js/Readability.js") + "\n" +
                     readResource(":/js/js/extract.js") + "\n})()";
    QVariant v = frame->evaluateJavaScript(script);
    QString json = v.toString();
    if (json.isEmpty()) { fail("Could not read the page (no result from page script)"); return false; }
    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &perr);
    if (!doc.isObject()) { fail("Could not read the page: " + perr.errorString()); return false; }
    QJsonObject o = doc.object();
    if (o.contains("readabilityError"))
        qWarning() << "epub: Readability failed, using plain page:" << o.value("readabilityError").toString();
    if (o.contains("error")) { fail("Could not read the page: " + o.value("error").toString()); return false; }
    m_title = o.value("title").toString().simplified();
    if (m_title.isEmpty()) m_title = frame->title().simplified();
    if (m_title.isEmpty()) m_title = m_pageUrl.host();
    m_byline = o.value("byline").toString().simplified();
    m_siteName = o.value("siteName").toString().simplified();
    m_excerpt = o.value("excerpt").toString().simplified();
    m_lang = o.value("lang").toString().simplified();
    if (m_lang.isEmpty()) m_lang = "en";
    m_fallback = o.value("fallback").toBool();
    m_content = o.value("content").toString();
    if (o.contains("url") && m_pageUrl.isEmpty()) m_pageUrl = QUrl(o.value("url").toString());
    if (m_content.trimmed().isEmpty()) { fail("Could not find any article text on this page"); return false; }
    // XMLSerializer on an HTML document can leave HTML-only entities behind.
    m_content.replace("&nbsp;", "&#160;");
    m_content.replace(QRegExp("&(?!(amp|lt|gt|quot|apos|#\\d+|#x[0-9a-fA-F]+);)"), "&amp;");
    return true;
}

// Find <img> tags, give each a local file name, and rewrite the tag to a clean XHTML one.
void EpubSaver::collectImages()
{
    QRegExp imgRe("<img\\b[^>]*>", Qt::CaseInsensitive);
    imgRe.setMinimal(true);
    QString out;
    int pos = 0, last = 0;
    while ((pos = imgRe.indexIn(m_content, last)) >= 0) {
        QString tag = imgRe.cap(0);
        int len = imgRe.matchedLength();
        out += m_content.mid(last, pos - last);
        last = pos + len;
        QString src = tagAttr(tag, "src");
        if (src.isEmpty()) src = tagAttr(tag, "data-src");
        QUrl url = src.startsWith("data:") ? QUrl(src) : m_pageUrl.resolved(QUrl(src));
        QString alt = tagAttr(tag, "alt");
        int idx = -1;
        for (int i = 0; i < m_images.size(); ++i) if (m_images[i].url == url) { idx = i; break; }
        if (idx < 0 && !src.isEmpty() && m_images.size() < kMaxImages &&
            (url.scheme() == "http" || url.scheme() == "https" || url.scheme() == "data")) {
            Image img;
            img.url = url;
            img.name = m_series ? QString("images/c%1img%2").arg(m_chapterIndex + 1).arg(m_images.size() + 1)
                                : QString("images/img%1").arg(m_images.size() + 1);   // extension added once fetched
            m_images << img;
            idx = m_images.size() - 1;
        }
        if (idx >= 0)
            out += QString("<img src=\"@@IMG%1@@\" alt=\"%2\"/>").arg(idx).arg(xmlEscape(alt));
        else if (!alt.isEmpty())
            out += "<span>[" + xmlEscape(alt) + "]</span>";
        // drop images we cannot fetch
    }
    out += m_content.mid(last);
    m_content = out;
}

void EpubSaver::fetchNext()
{
    ++m_current;
    if (m_current >= m_images.size()) { imagesDone(); return; }
    Image &img = m_images[m_current];
    emit progress(QString("Saving: image %1 of %2...").arg(m_current + 1).arg(m_images.size()));
    if (img.url.scheme() == "data") {
        // data:[<mediatype>][;base64],<data>
        QString s = img.url.toString();
        int comma = s.indexOf(',');
        if (comma > 0) {
            QString head = s.mid(5, comma - 5);
            QByteArray body = s.mid(comma + 1).toLatin1();
            QByteArray raw = head.contains(";base64") ? QByteArray::fromBase64(body) : QByteArray::fromPercentEncoding(body);
            storeImage(img, raw);
        }
        fetchNext();
        return;
    }
    QNetworkRequest req(img.url);
    req.setRawHeader("Referer", m_pageUrl.toEncoded());
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    m_reply = m_page->networkAccessManager()->get(req);
    connect(m_reply, SIGNAL(finished()), this, SLOT(imageFinished()));
    m_timer.start(kImageTimeoutMs);
}

void EpubSaver::imagesDone()
{
    // Substitute image placeholders in the content just extracted.
    for (int i = 0; i < m_images.size(); ++i) {
        QString ph = QString("@@IMG%1@@").arg(i);
        if (m_images[i].ok) m_content.replace(ph, xmlEscape(m_images[i].name));
        else {
            QRegExp tagRe("<img src=\"" + QRegExp::escape(ph) + "\"[^>]*/>");
            m_content.replace(tagRe, "");
        }
    }
    if (m_series) {
        Chapter ch;
        // A page with no real title (plain-text chapters report the host name) is named
        // after the link that led to it.
        QString t = m_title;
        if (t.isEmpty() || t.compare(m_pageUrl.host(), Qt::CaseInsensitive) == 0)
            t = m_links[m_chapterIndex].title;
        ch.title = t.isEmpty() ? QString("Chapter %1").arg(m_chapters.size() + 1) : t;
        ch.content = m_content;
        ch.url = m_pageUrl;
        ch.images = m_images;
        m_chapters << ch;
        loadNextChapter();
        return;
    }
    writeEpub();
}

void EpubSaver::imageFinished()
{
    m_timer.stop();
    QNetworkReply *r = m_reply;
    if (!r) return;
    m_reply = 0;
    r->deleteLater();
    Image &img = m_images[m_current];
    QVariant redirect = r->attribute(QNetworkRequest::RedirectionTargetAttribute);
    if (redirect.isValid() && r->error() == QNetworkReply::NoError) {
        QUrl next = r->url().resolved(redirect.toUrl());
        if (next != img.url && next.isValid()) {
            img.url = next;
            --m_current;       // fetch the same slot again with the new URL
            fetchNext();
            return;
        }
    }
    if (r->error() != QNetworkReply::NoError) {
        qWarning() << "epub: image failed" << img.url << r->errorString();
    } else {
        QByteArray raw = r->readAll();
        if (raw.size() > kMaxImageBytes) qWarning() << "epub: image too large, skipped" << img.url;
        else storeImage(img, raw);
    }
    fetchNext();
}

void EpubSaver::imageTimeout()
{
    if (!m_reply) return;
    qWarning() << "epub: image timed out" << m_images[m_current].url;
    QNetworkReply *r = m_reply;
    m_reply = 0;
    r->abort();
    r->deleteLater();
    fetchNext();
}

// Decode, downscale to the screen width, and store as JPEG/PNG. Unknown formats that
// Qt cannot decode are kept as-is when they are a common web format.
void EpubSaver::storeImage(Image &img, const QByteArray &raw)
{
    if (raw.isEmpty()) return;
    QBuffer buf;
    buf.setData(raw);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    QByteArray fmt = reader.format();
    bool isSvg = raw.left(512).contains("<svg");
    if (isSvg) {
        img.data = raw; img.mediaType = "image/svg+xml"; img.name += ".svg"; img.ok = true;
        return;
    }
    QSize size = reader.size();
    bool decoded = false;
    QImage image;
    if (size.isValid() && size.width() > 0) {
        if (size.width() > kMaxImageWidth)
            reader.setScaledSize(QSize(kMaxImageWidth, qMax(1, size.height() * kMaxImageWidth / size.width())));
        decoded = reader.read(&image);
        if (decoded && image.width() > kMaxImageWidth)
            image = image.scaledToWidth(kMaxImageWidth, Qt::SmoothTransformation);
    }
    if (decoded && !image.isNull()) {
        bool keepPng = (fmt == "png" || fmt == "gif") && image.hasAlphaChannel();
        QByteArray outData;
        QBuffer ob(&outData);
        ob.open(QIODevice::WriteOnly);
        if (keepPng) {
            if (image.save(&ob, "PNG")) { img.data = outData; img.mediaType = "image/png"; img.name += ".png"; img.ok = true; return; }
        }
        ob.close(); outData.clear(); ob.open(QIODevice::WriteOnly);
        if (image.format() == QImage::Format_Indexed8 || image.hasAlphaChannel())
            image = image.convertToFormat(QImage::Format_RGB32);
        if (image.save(&ob, "JPEG", 82)) { img.data = outData; img.mediaType = "image/jpeg"; img.name += ".jpg"; img.ok = true; return; }
        ob.close(); outData.clear(); ob.open(QIODevice::WriteOnly);
        if (image.save(&ob, "PNG")) { img.data = outData; img.mediaType = "image/png"; img.name += ".png"; img.ok = true; return; }
    }
    // Could not decode: keep the bytes if they look like a web image.
    if (raw.startsWith("\xFF\xD8")) { img.mediaType = "image/jpeg"; img.name += ".jpg"; }
    else if (raw.startsWith("\x89PNG")) { img.mediaType = "image/png"; img.name += ".png"; }
    else if (raw.startsWith("GIF8")) { img.mediaType = "image/gif"; img.name += ".gif"; }
    else { qWarning() << "epub: unsupported image format, skipped" << img.url << fmt; return; }
    img.data = raw;
    img.ok = true;
}

void EpubSaver::writeEpub()
{
    emit progress("Saving: writing EPUB...");
    if (m_series) {
        // One EPUB from the collected chapters: gather their images, use the first chapter's
        // metadata for the book-level fields.
        m_images.clear();
        for (int i = 0; i < m_chapters.size(); ++i) m_images += m_chapters[i].images;
        if (m_seriesTitle.isEmpty()) m_seriesTitle = m_chapters[0].title;
        m_title = m_seriesTitle;
        m_pageUrl = m_chapters[0].url;
        m_excerpt.clear();
    }
    QString t = m_title.isEmpty() ? QString("Saved page") : m_title;
    QString srcUrl = m_pageUrl.toString();
    QString date = QDateTime::currentDateTime().toString("yyyy-MM-dd");
    QString meta;
    if (!m_byline.isEmpty()) meta += "<p class=\"byline\">" + xmlEscape(m_byline) + "</p>\n";
    QString from = m_siteName.isEmpty() ? m_pageUrl.host() : m_siteName;
    meta += "<p class=\"source\">" + xmlEscape(from) + " &#183; saved " + date +
            "<br/><a href=\"" + xmlEscape(srcUrl) + "\">" + xmlEscape(srcUrl) + "</a></p>\n";

    QString xhtml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"" + xmlEscape(m_lang) + "\">\n<head>\n"
        "<title>" + xmlEscape(t) + "</title>\n"
        "<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/>\n"
        "</head>\n<body>\n"
        "<h1 class=\"title\">" + xmlEscape(t) + "</h1>\n" + meta +
        "<hr/>\n<div class=\"article\">\n" + m_content + "\n</div>\n</body>\n</html>\n";

    QString css =
        "body { line-height: 1.4; }\n"
        "h1.title { font-size: 1.6em; margin: 0 0 0.3em 0; }\n"
        "p.byline { font-style: italic; margin: 0; }\n"
        "p.source { font-size: 0.8em; color: #444; margin: 0.3em 0 0.8em 0; word-wrap: break-word; }\n"
        "img { max-width: 100%; height: auto; }\n"
        "figure { margin: 1em 0; text-align: center; }\n"
        "figcaption { font-size: 0.85em; font-style: italic; }\n"
        "pre { white-space: pre-wrap; font-size: 0.85em; }\n"
        "table { border-collapse: collapse; } td, th { border: 1px solid #999; padding: 0.2em 0.4em; }\n"
        "blockquote { margin: 1em 1.5em; font-style: italic; }\n";

    QString uid = "urn:uuid:" + QUuid::createUuid().toString().remove('{').remove('}');
    QString manifestImages;
    for (int i = 0; i < m_images.size(); ++i) {
        if (!m_images[i].ok) continue;
        manifestImages += "<item id=\"img" + QString::number(i) + "\" href=\"" + xmlEscape(m_images[i].name) +
                          "\" media-type=\"" + m_images[i].mediaType + "\"/>\n";
    }
    // Chapter documents (series) or the single page.
    QStringList chapterNames, chapterTitles, chapterDocs;
    if (m_series) {
        for (int i = 0; i < m_chapters.size(); ++i) {
            QString nm = QString("ch%1.xhtml").arg(i + 1, 3, 10, QChar('0'));
            QString ct = m_chapters[i].title;
            QString doc = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">\n"
                "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"" + xmlEscape(m_lang) + "\">\n<head>\n"
                "<title>" + xmlEscape(ct) + "</title>\n"
                "<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/>\n"
                "</head>\n<body>\n<h1 class=\"title\">" + xmlEscape(ct) + "</h1>\n"
                "<p class=\"source\"><a href=\"" + xmlEscape(m_chapters[i].url.toString()) + "\">" +
                xmlEscape(m_chapters[i].url.toString()) + "</a></p>\n<hr/>\n<div class=\"article\">\n" +
                m_chapters[i].content + "\n</div>\n</body>\n</html>\n";
            chapterNames << nm; chapterTitles << ct; chapterDocs << doc;
        }
    } else {
        chapterNames << "page.xhtml"; chapterTitles << t; chapterDocs << xhtml;
    }
    QString manifestDocs, spine, navPoints;
    for (int i = 0; i < chapterNames.size(); ++i) {
        manifestDocs += "<item id=\"doc" + QString::number(i) + "\" href=\"" + chapterNames[i] +
                        "\" media-type=\"application/xhtml+xml\"/>\n";
        spine += "<itemref idref=\"doc" + QString::number(i) + "\"/>";
        navPoints += "<navPoint id=\"p" + QString::number(i + 1) + "\" playOrder=\"" + QString::number(i + 1) +
                     "\"><navLabel><text>" + xmlEscape(chapterTitles[i]) + "</text></navLabel><content src=\"" +
                     chapterNames[i] + "\"/></navPoint>";
    }

    QString opf = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"uid\" version=\"2.0\">\n"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:opf=\"http://www.idpf.org/2007/opf\">\n"
        "<dc:title>" + xmlEscape(t) + "</dc:title>\n"
        "<dc:creator opf:role=\"aut\">" + xmlEscape(m_byline.isEmpty() ? from : m_byline) + "</dc:creator>\n"
        "<dc:language>" + xmlEscape(m_lang) + "</dc:language>\n"
        "<dc:identifier id=\"uid\">" + uid + "</dc:identifier>\n"
        "<dc:source>" + xmlEscape(srcUrl) + "</dc:source>\n"
        "<dc:publisher>" + xmlEscape(from) + "</dc:publisher>\n"
        "<dc:date>" + date + "</dc:date>\n" +
        (m_excerpt.isEmpty() ? QString() : "<dc:description>" + xmlEscape(m_excerpt) + "</dc:description>\n") +
        "</metadata>\n"
        "<manifest>\n" + manifestDocs +
        "<item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>\n"
        "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n" + manifestImages +
        "</manifest>\n"
        "<spine toc=\"ncx\">" + spine + "</spine>\n"
        "</package>\n";
    QString ncx = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n"
        "<head><meta name=\"dtb:uid\" content=\"" + uid + "\"/>"
        "<meta name=\"dtb:depth\" content=\"1\"/><meta name=\"dtb:totalPageCount\" content=\"0\"/>"
        "<meta name=\"dtb:maxPageNumber\" content=\"0\"/></head>\n"
        "<docTitle><text>" + xmlEscape(t) + "</text></docTitle>\n"
        "<navMap>" + navPoints + "</navMap>\n</ncx>\n";
    QString container = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
        "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n"
        "</container>\n";

    QList<Entry> entries;
    Entry e;
    e.name = "mimetype"; e.data = "application/epub+zip"; entries << e;
    e.name = "META-INF/container.xml"; e.data = container.toUtf8(); entries << e;
    e.name = "OEBPS/content.opf"; e.data = opf.toUtf8(); entries << e;
    e.name = "OEBPS/toc.ncx"; e.data = ncx.toUtf8(); entries << e;
    e.name = "OEBPS/style.css"; e.data = css.toUtf8(); entries << e;
    for (int i = 0; i < chapterNames.size(); ++i) { e.name = ("OEBPS/" + chapterNames[i]).toUtf8(); e.data = chapterDocs[i].toUtf8(); entries << e; }
    for (int i = 0; i < m_images.size(); ++i) {
        if (!m_images[i].ok) continue;
        e.name = ("OEBPS/" + m_images[i].name).toUtf8(); e.data = m_images[i].data; entries << e;
    }
    QByteArray zip = buildZip(entries);

    QDir dir(m_outDir);
    if (!dir.exists() && !dir.mkpath(".")) { fail("Cannot create folder " + m_outDir); return; }
    QString base = safeFileName(t);
    QString path = dir.filePath(base + ".epub");
    for (int n = 2; QFile::exists(path) && n < 100; ++n) path = dir.filePath(base + QString(" (%1).epub").arg(n));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { fail("Cannot write " + path + ": " + f.errorString()); return; }
    if (f.write(zip) != zip.size()) { f.close(); QFile::remove(path); fail("Write failed (disk full?)"); return; }
    f.close();
    if (m_library) appendHandoff("saved", path);
    ::sync();
    qDebug() << "epub: wrote" << path << zip.size() << "bytes," << m_images.size() << "images," << chapterNames.size() << "documents";
    m_running = false;
    emit finished(true, path);
}

// ------------------------------------------------------------- handoff

QString EpubSaver::handoffFile()
{
    const char *env = getenv("KB_HANDOFF_FILE");
    if (env && *env) return QString::fromLocal8Bit(env);
    return QDir(QCoreApplication::applicationDirPath()).filePath("epub-handoff.txt");
}

bool EpubSaver::appendHandoff(const QString &kind, const QString &path)
{
    QFile f(handoffFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        qWarning() << "epub: cannot write handoff file" << f.fileName() << f.errorString();
        return false;
    }
    QTextStream ts(&f);
    ts.setCodec("UTF-8");
    ts << kind << '\t' << path << '\n';
    ts.flush();
    f.close();
    return true;
}

// ------------------------------------------------------------- SavedDialog

SavedDialog::SavedDialog(const QString &epubPath, QWidget *parent)
    : QWidget(parent), m_path(epubPath)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setAutoFillBackground(false);

    m_box = new QFrame(this);
    static_cast<QFrame *>(m_box)->setFrameStyle(QFrame::Box | QFrame::Plain);
    static_cast<QFrame *>(m_box)->setLineWidth(3);
    m_box->setAutoFillBackground(true);
    QPalette pal = m_box->palette();
    pal.setColor(QPalette::Window, Qt::white);
    pal.setColor(QPalette::WindowText, Qt::black);
    m_box->setPalette(pal);

    QVBoxLayout *v = new QVBoxLayout(m_box);
    v->setContentsMargins(30, 30, 30, 30);
    v->setSpacing(24);
    QFont f; f.setPixelSize(38);
    QFont fs; fs.setPixelSize(30);

    QLabel *head = new QLabel("Saved to the library");
    head->setFont(f);
    head->setAlignment(Qt::AlignCenter);
    v->addWidget(head);

    QString shown = epubPath;
    if (shown.startsWith("/mnt/onboard/")) shown = shown.mid(13);
    QLabel *name = new QLabel(shown);
    name->setFont(fs);
    name->setWordWrap(true);
    name->setAlignment(Qt::AlignCenter);
    v->addWidget(name);

    QLabel *q = new QLabel("Open it in KOReader now? The browser will close.");
    q->setFont(fs);
    q->setWordWrap(true);
    q->setAlignment(Qt::AlignCenter);
    v->addWidget(q);

    QHBoxLayout *row = new QHBoxLayout;
    row->setSpacing(20);
    QPushButton *later = new QPushButton("Keep browsing");
    QPushButton *open = new QPushButton("Open now");
    foreach (QPushButton *b, QList<QPushButton *>() << later << open) {
        b->setFont(f);
        b->setMinimumSize(300, 100);
        b->setFocusPolicy(Qt::NoFocus);
        b->setProperty("kb_fast", true);
    }
    row->addWidget(later);
    row->addWidget(open);
    v->addLayout(row);

    connect(later, SIGNAL(clicked()), this, SLOT(laterClicked()));
    connect(open, SIGNAL(clicked()), this, SLOT(openClicked()));

    if (parent) resize(parent->size());
    place();
    raise();
    show();
}

void SavedDialog::place()
{
    int w = qMin(width() - 40, 900);
    m_box->setFixedWidth(w);
    m_box->adjustSize();
    m_box->move((width() - m_box->width()) / 2, (height() - m_box->height()) / 2);
}

void SavedDialog::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    place();
}

// Taps outside the box do nothing (the overlay swallows them).
void SavedDialog::mousePressEvent(QMouseEvent *e) { e->accept(); }

void SavedDialog::openClicked()
{
    EpubSaver::appendHandoff("open", m_path);
    emit openRequested(m_path);
    close();
}

void SavedDialog::laterClicked()
{
    emit dismissed();
    close();
}


// ------------------------------------------------------------- SaveChoiceDialog

SaveChoiceDialog::SaveChoiceDialog(int chapterCount, const QString &seriesTitle, QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setAutoFillBackground(false);
    m_box = new QFrame(this);
    static_cast<QFrame *>(m_box)->setFrameStyle(QFrame::Box | QFrame::Plain);
    static_cast<QFrame *>(m_box)->setLineWidth(3);
    m_box->setAutoFillBackground(true);
    QPalette pal = m_box->palette();
    pal.setColor(QPalette::Window, Qt::white);
    pal.setColor(QPalette::WindowText, Qt::black);
    m_box->setPalette(pal);

    QVBoxLayout *v = new QVBoxLayout(m_box);
    v->setContentsMargins(30, 30, 30, 30);
    v->setSpacing(24);
    QFont f; f.setPixelSize(38);
    QFont fs; fs.setPixelSize(30);

    QLabel *head = new QLabel("Save to the library");
    head->setFont(f); head->setAlignment(Qt::AlignCenter);
    v->addWidget(head);
    QLabel *q = new QLabel(QString("This page links to %1 parts of a series%2. Save just this page, or all of them as one book with a chapter list?")
                           .arg(chapterCount).arg(seriesTitle.isEmpty() ? QString() : " (" + seriesTitle + ")"));
    q->setFont(fs); q->setWordWrap(true); q->setAlignment(Qt::AlignCenter);
    v->addWidget(q);

    QPushButton *series = new QPushButton(QString("Whole series (%1 parts)").arg(chapterCount));
    QPushButton *page = new QPushButton("Just this page");
    QPushButton *cancel = new QPushButton("Cancel");
    foreach (QPushButton *b, QList<QPushButton *>() << series << page << cancel) {
        b->setFont(f);
        b->setMinimumSize(300, 100);
        b->setFocusPolicy(Qt::NoFocus);
        b->setProperty("kb_fast", true);
        v->addWidget(b);
    }
    connect(series, SIGNAL(clicked()), this, SIGNAL(seriesChosen()));
    connect(page, SIGNAL(clicked()), this, SIGNAL(pageChosen()));
    connect(cancel, SIGNAL(clicked()), this, SIGNAL(cancelled()));
    connect(series, SIGNAL(clicked()), this, SLOT(close()));
    connect(page, SIGNAL(clicked()), this, SLOT(close()));
    connect(cancel, SIGNAL(clicked()), this, SLOT(close()));

    if (parent) resize(parent->size());
    place();
    raise();
    show();
}

void SaveChoiceDialog::place()
{
    int w = qMin(width() - 40, 900);
    m_box->setFixedWidth(w);
    m_box->adjustSize();
    m_box->move((width() - m_box->width()) / 2, (height() - m_box->height()) / 2);
}

void SaveChoiceDialog::resizeEvent(QResizeEvent *e) { QWidget::resizeEvent(e); place(); }
void SaveChoiceDialog::mousePressEvent(QMouseEvent *e) { e->accept(); }
