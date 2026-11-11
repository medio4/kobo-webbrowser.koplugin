#include "adblock.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
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

static QString offMarkPath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath("adblock-off.txt");
}

AdBlock *AdBlock::instance()
{
    static AdBlock *a = new AdBlock(QCoreApplication::instance());
    return a;
}

AdBlock::AdBlock(QObject *parent)
    : QObject(parent), m_enabled(!getenv("KB_NO_ADBLOCK") && !QFile::exists(offMarkPath())),
      m_loaded(false), m_log(getenv("KB_ADBLOCK_LOG") != 0), m_blocked(0)
{
}

void AdBlock::load()
{
    m_loaded = true;
    QElapsedTimer t; t.start();
    const char *env = getenv("KB_ADBLOCK_LIST");
    QString path = (env && *env) ? QString::fromLocal8Bit(env)
                                 : QDir(QCoreApplication::applicationDirPath()).filePath("adblock-hosts.txt");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qDebug() << "adblock: no list at" << path;
        return;
    }
    QByteArray data = f.readAll();
    m_hashes.reserve(data.count('\n') + 1);
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
        while (sp < b && *sp != ' ' && *sp != '\t') ++sp;
        if (sp < b) {
            a = sp;
            while (a < b && (*a == ' ' || *a == '\t')) ++a;
            sp = a;
            while (sp < b && *sp != ' ' && *sp != '\t' && *sp != '\r' && *sp != '#') ++sp;
        }
        while (sp > a && sp[-1] == '\r') --sp;
        if (sp > a) m_hashes.append(hostHash(a, sp - a));
    }
    std::sort(m_hashes.begin(), m_hashes.end());
    m_hashes.erase(std::unique(m_hashes.begin(), m_hashes.end()), m_hashes.end());
    m_hashes.squeeze();
    qDebug() << "adblock:" << m_hashes.size() << "hosts loaded in" << t.elapsed() << "ms from" << path;
}

bool AdBlock::shouldBlock(const QUrl &url)
{
    if (!m_enabled) return false;
    QString scheme = url.scheme();
    if (scheme != "http" && scheme != "https") return false;
    if (url == m_navUrl) return false;
    if (!m_loaded) load();
    if (m_hashes.isEmpty()) return false;
    QByteArray host = url.host().toLatin1();   // QUrl gives the ACE (punycode) form, lowercase
    const char *s = host.constData();
    int n = host.size();
    if (n && s[n - 1] == '.') --n;
    // Check the host itself, then each parent domain down to two labels:
    // a.b.example.com, b.example.com, example.com
    int i = 0;
    for (;;) {
        const char *dot = (const char *)memchr(s + i, '.', n - i);
        if (!dot) break;   // single label left: never a listed entry
        if (std::binary_search(m_hashes.constBegin(), m_hashes.constEnd(), hostHash(s + i, n - i))) {
            ++m_blocked;
            if (m_log) qDebug() << "adblock: blocked" << url.toString().left(120);
            return true;
        }
        i = dot - s + 1;
    }
    return false;
}

void AdBlock::setEnabled(bool on)
{
    if (on == m_enabled) return;
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
