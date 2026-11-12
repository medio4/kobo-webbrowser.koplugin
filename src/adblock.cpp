#include "adblock.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>
#include <QtCore/QDebug>
#include <algorithm>
#include <cstdlib>
#include <cstring>

// FNV-1a, 64 bit. 42k entries in a 2^64 space: a false match is not a practical concern.
static quint64 hostHash(const char *s, int n)
{
    quint64 h = Q_UINT64_C(14695981039346656037);
    for (int i = 0; i < n; ++i) {
        unsigned char c = s[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        h ^= c;
        h *= Q_UINT64_C(1099511628211);
    }
    return h;
}

static QString binFile(const char *name)
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QString::fromLatin1(name));
}

static QString offMarkPath() { return binFile("adblock-off.txt"); }

// Hashes every host in a hosts-style or one-domain-per-line file (sorted, unique).
static QVector<quint64> readHostFile(const QString &path)
{
    QVector<quint64> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return out;
    QByteArray data = f.readAll();
    out.reserve(data.count('\n') + 1);
    const char *p = data.constData(), *end = p + data.size();
    while (p < end) {
        const char *eol = (const char *)memchr(p, '\n', end - p);
        if (!eol) eol = end;
        const char *a = p, *b = eol;
        p = eol + 1;
        while (a < b && (*a == ' ' || *a == '\t')) ++a;
        if (a == b || *a == '#') continue;
        // hosts format: "0.0.0.0 host" / "127.0.0.1 host" -> take the second field
        const char *sp = a;
        while (sp < b && *sp != ' ' && *sp != '\t' && *sp != '\r' && *sp != '#') ++sp;
        const char *q = sp;
        while (q < b && (*q == ' ' || *q == '\t')) ++q;
        if (q < b && *q != '#' && *q != '\r') {
            a = q;
            sp = a;
            while (sp < b && *sp != ' ' && *sp != '\t' && *sp != '\r' && *sp != '#') ++sp;
        }
        // Tolerate pasted addresses: "https://www.example.com/path" -> "www.example.com"
        const char *scheme = (const char *)memchr(a, ':', sp - a);
        if (scheme && sp - scheme > 3 && scheme[1] == '/' && scheme[2] == '/') a = scheme + 3;
        const char *slash = (const char *)memchr(a, '/', sp - a);
        if (slash) sp = slash;
        while (sp > a && sp[-1] == '.') --sp;
        if (sp > a) out.append(hostHash(a, sp - a));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    out.squeeze();
    return out;
}

// Does the host or one of its parent domains (down to two labels) hash into set?
static bool hostListed(const QVector<quint64> &set, const char *s, int n)
{
    if (set.isEmpty()) return false;
    int i = 0;
    for (;;) {
        const char *dot = (const char *)memchr(s + i, '.', n - i);
        if (!dot) return false;   // single label left: never a listed entry
        if (std::binary_search(set.constBegin(), set.constEnd(), hostHash(s + i, n - i))) return true;
        i = dot - s + 1;
    }
}

static qint64 fileStamp(const QString &path)
{
    QFileInfo fi(path);
    return fi.exists() ? fi.lastModified().toMSecsSinceEpoch() ^ (fi.size() << 40) : -1;
}

AdBlock *AdBlock::instance()
{
    static AdBlock *a = new AdBlock(QCoreApplication::instance());
    return a;
}

AdBlock::AdBlock(QObject *parent)
    : QObject(parent), m_extraStamp(-2), m_allowStamp(-2), m_envOff(getenv("KB_NO_ADBLOCK") != 0),
      m_enabled(false), m_loaded(false), m_log(getenv("KB_ADBLOCK_LOG") != 0), m_blocked(0)
{
    refresh();
}

void AdBlock::load()
{
    m_loaded = true;
    QElapsedTimer t; t.start();
    const char *env = getenv("KB_ADBLOCK_LIST");
    QString path = (env && *env) ? QString::fromLocal8Bit(env) : binFile("adblock-hosts.txt");
    m_hashes = readHostFile(path);
    qDebug() << "adblock:" << m_hashes.size() << "hosts loaded in" << t.elapsed() << "ms from" << path;
}

void AdBlock::refresh()
{
    bool on = !m_envOff && !QFile::exists(offMarkPath());
    if (on != m_enabled) {
        m_enabled = on;
        qDebug() << "adblock:" << (on ? "on" : "off") << "(settings)";
        emit enabledChanged(on);
    }
    QString extra = binFile("adblock-extra.txt"), allow = binFile("adblock-allow.txt");
    qint64 st = fileStamp(extra);
    if (st != m_extraStamp) {
        m_extraStamp = st;
        m_extra = readHostFile(extra);
        qDebug() << "adblock:" << m_extra.size() << "extra sites to block";
    }
    st = fileStamp(allow);
    if (st != m_allowStamp) {
        m_allowStamp = st;
        m_allow = readHostFile(allow);
        qDebug() << "adblock:" << m_allow.size() << "allowed sites";
    }
}

void AdBlock::allowNavigation(const QUrl &url)
{
    m_navUrl = url;
    m_pageHost = url.host().toLatin1();
    if (m_pageHost.endsWith('.')) m_pageHost.chop(1);
}

bool AdBlock::shouldBlock(const QUrl &url)
{
    if (!m_enabled) return false;
    QString scheme = url.scheme();
    if (scheme != "http" && scheme != "https") return false;
    if (url == m_navUrl) return false;
    if (!m_loaded) load();
    QByteArray host = url.host().toLatin1();   // QUrl gives the ACE (punycode) form, lowercase
    int n = host.size();
    if (n && host[n - 1] == '.') --n;
    const char *s = host.constData();
    if (!hostListed(m_hashes, s, n) && !hostListed(m_extra, s, n)) return false;
    // Allowed: the request's own host, or the page it is for.
    if (hostListed(m_allow, s, n) || hostListed(m_allow, m_pageHost.constData(), m_pageHost.size()))
        return false;
    ++m_blocked;
    if (m_log) qDebug() << "adblock: blocked" << url.toString().left(120);
    return true;
}

void AdBlock::setEnabled(bool on)
{
    if (on == m_enabled || (on && m_envOff)) return;
    m_enabled = on;
    if (on) QFile::remove(offMarkPath());
    else { QFile f(offMarkPath()); if (f.open(QIODevice::WriteOnly)) f.write("ad blocking turned off in the browser\n"); }
    qDebug() << "adblock:" << (on ? "on" : "off");
    emit enabledChanged(on);
}

// ---------------------------------------------------------------------------

BlockedReply::BlockedReply(const QNetworkRequest &req, QNetworkAccessManager::Operation op, QObject *parent)
    : QNetworkReply(parent)
{
    setRequest(req);
    setUrl(req.url());
    setOperation(op);
    QIODevice::open(QIODevice::ReadOnly);
    // Finish from the event loop: WebKit connects to the reply after createRequest returns.
    QTimer::singleShot(0, this, SLOT(fail()));
}

void BlockedReply::fail()
{
    setError(ContentAccessDenied, "Blocked by the ad blocker");
    emit error(ContentAccessDenied);
    setFinished(true);
    emit finished();
}
