#include "download.h"

#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegExp>
#include <QtCore/QStringList>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <unistd.h>

// Formats KOReader opens (its document registry: CREngine, MuPDF, DjVuLibre).
static const char *const kBookExts[] = {
    "epub", "pdf", "djvu", "djv", "cbz", "cbt", "fb2", "fbz", "mobi", "azw", "azw3", "prc",
    "pdb", "rtf", "doc", "docx", "odt", "xps", "oxps", "chm", "txt",
};

static bool isBookExt(const QString &ext)
{
    for (unsigned i = 0; i < sizeof kBookExts / sizeof kBookExts[0]; ++i)
        if (ext == QLatin1String(kBookExts[i])) return true;
    return false;
}

Downloader::Downloader(QNetworkAccessManager *nam, const QString &outDir, QObject *parent)
    : QObject(parent), m_nam(nam), m_outDir(outDir), m_reply(0), m_file(0), m_decided(false),
      m_fetched(false), m_redirects(0), m_lastShown(-1)
{
}

Downloader::~Downloader()
{
    cancel();
}

QString Downloader::bookExtension(const QString &fileName, const QString &contentType)
{
    QString n = fileName.toLower();
    if (n.endsWith(".fb2.zip")) return "fb2.zip";
    int dot = n.lastIndexOf('.');
    if (dot > 0) {
        QString ext = n.mid(dot + 1);
        if (ext != "txt" && isBookExt(ext)) return ext;   // a .txt name alone could be anything
    }
    QString ct = contentType.section(';', 0, 0).trimmed().toLower();
    if (ct == "application/pdf" || ct == "application/x-pdf") return "pdf";
    if (ct == "application/epub+zip") return "epub";
    if (ct == "application/vnd.comicbook+zip" || ct == "application/x-cbz") return "cbz";
    if (ct == "application/x-cbt" || ct == "application/vnd.comicbook+tar") return "cbt";
    if (ct == "image/vnd.djvu" || ct == "image/x-djvu" || ct == "image/djvu") return "djvu";
    if (ct == "application/x-fictionbook+xml" || ct == "application/x-fictionbook") return "fb2";
    if (ct == "application/x-mobipocket-ebook") return "mobi";
    if (ct == "application/vnd.amazon.ebook" || ct == "application/vnd.amazon.mobi8-ebook") return "azw3";
    if (ct == "application/rtf" || ct == "text/rtf") return "rtf";
    if (ct == "application/vnd.openxmlformats-officedocument.wordprocessingml.document") return "docx";
    if (ct == "application/vnd.oasis.opendocument.text") return "odt";
    if (ct == "application/oxps" || ct == "application/vnd.ms-xpsdocument") return "xps";
    if (ct == "application/vnd.ms-htmlhelp") return "chm";
    // A generic type: trust a .txt name now.
    if (dot > 0 && n.mid(dot + 1) == "txt") return "txt";
    return QString();
}

QString Downloader::suggestedName(const QByteArray &contentDisposition, const QUrl &url)
{
    QString name;
    QString cd = QString::fromUtf8(contentDisposition);
    // RFC 5987: filename*=UTF-8''Some%20Book.pdf
    QRegExp star("filename\\*\\s*=\\s*([^']*)'[^']*'([^;]+)", Qt::CaseInsensitive);
    QRegExp plain("filename\\s*=\\s*(\"([^\"]*)\"|([^;]+))", Qt::CaseInsensitive);
    if (star.indexIn(cd) >= 0) {
        QByteArray raw = QByteArray::fromPercentEncoding(star.cap(2).trimmed().toLatin1());
        name = star.cap(1).trimmed().toLower() == "utf-8" ? QString::fromUtf8(raw) : QString::fromLatin1(raw);
    } else if (plain.indexIn(cd) >= 0) {
        name = !plain.cap(2).isEmpty() ? plain.cap(2) : plain.cap(3).trimmed();
    }
    if (name.isEmpty()) name = QUrl::fromPercentEncoding(url.path().section('/', -1).toUtf8());
    name = name.section('/', -1).section('\\', -1).trimmed();
    return name;
}

QString Downloader::uniquePath(const QString &dir, const QString &base, const QString &ext)
{
    QDir d(dir);
    QString p = d.filePath(base + "." + ext);
    for (int i = 2; QFile::exists(p) || QFile::exists(p + ".part"); ++i)
        p = d.filePath(QString("%1 (%2).%3").arg(base).arg(i).arg(ext));
    return p;
}

bool Downloader::take(QNetworkReply *reply, QString *whyNot)
{
    if (!reply) return false;
    if (m_reply) {   // one at a time
        if (whyNot) *whyNot = "a download is already running";
        reply->abort();
        reply->deleteLater();
        return false;
    }
    m_fetched = false;
    m_redirects = 0;
    attach(reply);
    if (!decide(whyNot)) {
        cleanup();
        return false;
    }
    if (reply->isFinished()) QMetaObject::invokeMethod(this, "replyFinished", Qt::QueuedConnection);
    else readMore();
    return true;
}

void Downloader::fetch(const QNetworkRequest &request)
{
    if (m_reply) { emit finished(false, "a download is already running"); return; }
    m_fetched = true;
    m_redirects = 0;
    QNetworkRequest req(request);
    req.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    attach(m_nam->get(req));
}

void Downloader::attach(QNetworkReply *reply)
{
    m_reply = reply;
    m_decided = false;
    m_lastShown = -1;
    connect(reply, SIGNAL(metaDataChanged()), this, SLOT(metaData()));
    connect(reply, SIGNAL(readyRead()), this, SLOT(readMore()));
    connect(reply, SIGNAL(finished()), this, SLOT(replyFinished()));
    connect(reply, SIGNAL(downloadProgress(qint64,qint64)), this, SLOT(received(qint64,qint64)));
}

// Picks the file name and opens "<name>.part" once the headers are in.
bool Downloader::decide(QString *whyNot)
{
    m_decided = true;
    QString type = m_reply->header(QNetworkRequest::ContentTypeHeader).toString();
    QString name = suggestedName(m_reply->rawHeader("Content-Disposition"), m_reply->url());
    QString ext = type.startsWith("text/html", Qt::CaseInsensitive) ? QString() : bookExtension(name, type);
    if (ext.isEmpty()) {
        QString what = type.section(';', 0, 0).trimmed();
        if (whyNot) *whyNot = what.isEmpty() ? QString("KOReader can't open this file")
                                             : QString("KOReader can't open %1 files").arg(what);
        qDebug() << "download: not a book:" << m_reply->url().toString().left(120) << type << name;
        return false;
    }
    QString base = name;
    if (base.toLower().endsWith("." + ext)) base.chop(ext.size() + 1);
    base = base.simplified();
    base.replace(QRegExp("[\\\\/:*?\"<>|\\x00-\\x1f]"), "-");   // as EpubSaver::safeFileName
    base.replace(QRegExp("^[.\\s-]+"), "");
    if (base.size() > 80) base = base.left(80).trimmed();
    if (base.isEmpty()) base = m_reply->url().host().isEmpty() ? QString("Download") : m_reply->url().host();

    QDir dir(m_outDir);
    if (!dir.exists() && !dir.mkpath(".")) {
        if (whyNot) *whyNot = "can't create " + m_outDir;
        return false;
    }
    m_finalPath = uniquePath(m_outDir, base, ext);
    m_name = QFileInfo(m_finalPath).fileName();
    m_file = new QFile(m_finalPath + ".part");
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (whyNot) *whyNot = "can't write " + m_file->fileName() + ": " + m_file->errorString();
        delete m_file;
        m_file = 0;
        return false;
    }
    qDebug() << "download:" << m_reply->url().toString().left(120) << type << "->" << m_finalPath;
    emit progress("Downloading " + m_name + "...");
    return true;
}

void Downloader::metaData()
{
    if (!m_reply || !m_fetched || m_decided) return;
    QVariant redirect = m_reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
    if (redirect.isValid()) return;   // followed in replyFinished
    int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 400) return;        // reported in replyFinished
    QString why;
    if (!decide(&why)) fail(why);
}

void Downloader::readMore()
{
    if (!m_reply || !m_file) return;
    QByteArray d = m_reply->readAll();
    if (d.isEmpty()) return;
    if (m_file->write(d) != d.size()) fail("can't write " + m_name + ": " + m_file->errorString());
}

void Downloader::received(qint64 done, qint64 total)
{
    if (!m_file) return;
    // Few status updates: each 10% when the size is known, else each megabyte.
    if (total > 0) {
        int pct = int(done * 100 / total) / 10 * 10;
        if (pct == m_lastShown) return;
        m_lastShown = pct;
        emit progress(QString("Downloading %1: %2%").arg(m_name).arg(pct));
    } else {
        int mb = int(done >> 20);
        if (mb == m_lastShown) return;
        m_lastShown = mb;
        emit progress(QString("Downloading %1: %2 MB").arg(m_name).arg(mb));
    }
}

void Downloader::replyFinished()
{
    QNetworkReply *r = m_reply;
    if (!r) return;
    if (m_fetched && !m_decided) {
        QUrl target = r->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        if (target.isValid() && r->error() == QNetworkReply::NoError) {
            if (++m_redirects > 8) { fail("too many redirects"); return; }
            QNetworkRequest req(r->request());
            req.setUrl(r->url().resolved(target));
            disconnect(r, 0, this, 0);
            r->deleteLater();
            m_reply = 0;
            attach(m_nam->get(req));
            return;
        }
    }
    if (r->error() != QNetworkReply::NoError) {
        if (r->error() != QNetworkReply::OperationCanceledError) fail(r->errorString());
        return;
    }
    int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 400) { fail(QString("the site answered with an error (HTTP %1)").arg(status)); return; }
    if (!m_decided) {
        QString why;
        if (!decide(&why)) { fail(why); return; }
    }
    readMore();
    if (!m_file) return;   // a write failed
    qint64 size = m_file->size();
    m_file->close();
    if (size == 0) { fail("the file was empty"); return; }
    QString part = m_file->fileName();
    if (QFile::exists(m_finalPath)) {   // appeared meanwhile: pick another name
        QFileInfo fi(m_finalPath);
        QString ext = m_finalPath.endsWith(".fb2.zip") ? QString("fb2.zip") : fi.suffix();
        QString base = fi.fileName();
        base.chop(ext.size() + 1);
        m_finalPath = uniquePath(fi.absolutePath(), base, ext);
    }
    if (!QFile::rename(part, m_finalPath)) { fail("can't rename " + part); return; }
    delete m_file;
    m_file = 0;
    ::sync();
    qDebug() << "download: wrote" << m_finalPath << size << "bytes";
    QString path = m_finalPath;
    cleanup();
    emit finished(true, path);
}

void Downloader::fail(const QString &why)
{
    qWarning() << "download failed:" << why << (m_reply ? m_reply->url().toString().left(120) : QString());
    cleanup();
    emit finished(false, why);
}

void Downloader::cancel()
{
    if (!m_reply) return;
    cleanup();
}

// Drops the reply and any partial file.
void Downloader::cleanup()
{
    if (m_reply) {
        disconnect(m_reply, 0, this, 0);
        if (!m_reply->isFinished()) m_reply->abort();
        m_reply->deleteLater();
        m_reply = 0;
    }
    if (m_file) {
        m_file->close();
        m_file->remove();
        delete m_file;
        m_file = 0;
    }
    m_decided = false;
}
