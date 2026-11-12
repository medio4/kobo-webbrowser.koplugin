// Desktop page-load check for the CSS compatibility layer, with the distro's QtWebKit
// (newer than the Kobo's, so it shows breakage and cost, not the Kobo's rendering).
// For each URL: load with the layer on and off (alternating, 3 times each), compare the
// number of CSS rules WebKit parsed and the page's text length, and sum the rewrite time.
#include "../../src/memtune.h"
#include <QtWidgets/QApplication>
#include <QtWebKitWidgets/QWebPage>
#include <QtWebKitWidgets/QWebFrame>
#include <QtCore/QEventLoop>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>
#include <cstdio>
#include <cstdlib>

static qint64 g_us = 0;
static int g_replies = 0;
static void handler(QtMsgType, const QMessageLogContext &, const QString &msg)
{
    if (getenv("VERBOSE") && getenv("ALLMSG")) fprintf(stderr, "msg: %s\n", qPrintable(msg));
    int i = msg.indexOf(" us ");
    if (msg.startsWith("csscompat:") && i > 0) {
        g_us += msg.left(i).section(' ', -1).toLongLong();
        ++g_replies;
        if (getenv("VERBOSE")) fprintf(stderr, "  %s\n", qPrintable(msg));
    }
}

struct Result { qint64 ms; int rules, text; };

static Result load(const QUrl &url, bool compat)
{
    if (compat) unsetenv("KB_NO_CSS_COMPAT"); else setenv("KB_NO_CSS_COMPAT", "1", 1);
    QWebSettings::clearMemoryCaches();   // or WebKit serves the page and its sheets from memory
    QWebPage page;
    page.setNetworkAccessManager(new KoboNetworkAccessManager(&page));
    page.setViewportSize(QSize(536, 724));
    QEventLoop loop;
    QObject::connect(&page, SIGNAL(loadFinished(bool)), &loop, SLOT(quit()));
    QTimer::singleShot(60000, &loop, SLOT(quit()));
    QElapsedTimer t; t.start();
    page.mainFrame()->load(url);
    loop.exec();
    Result r;
    r.ms = t.elapsed();
    r.rules = page.mainFrame()->evaluateJavaScript(
        "var n=0;for(var i=0;i<document.styleSheets.length;i++){try{n+=document.styleSheets[i].cssRules.length}catch(e){}}n").toInt();
    r.text = page.mainFrame()->evaluateJavaScript("document.body?document.body.innerText.length:0").toInt();
    return r;
}

int main(int argc, char **argv)
{
    setenv("KB_CSS_COMPAT_LOG", "1", 1);
    QApplication app(argc, argv);
    qInstallMessageHandler(handler);
    QWebSettings::globalSettings()->setMaximumPagesInCache(0);
    QWebSettings::setObjectCacheCapacities(0, 0, 0);
    for (int k = 1; k < argc; ++k) {
        QUrl url(QString::fromLocal8Bit(argv[k]));
        qint64 on = 0, off = 0, us = 0;
        int replies = 0;
        Result a = {0, 0, 0}, b = {0, 0, 0};
        load(url, false);   // warm up DNS and the server's caches
        for (int rep = 0; rep < 3; ++rep) {
            g_us = 0; g_replies = 0;
            if (rep % 2) { b = load(url, false); off += b.ms; }
            a = load(url, true); on += a.ms; us += g_us; replies = g_replies;
            if (!(rep % 2)) { b = load(url, false); off += b.ms; }
            if (getenv("VERBOSE")) fprintf(stderr, "  run %d: on %lld ms, off %lld ms\n", rep, a.ms, b.ms);
        }
        printf("%s\n  load on %lld ms / off %lld ms (avg of 3, network included)\n"
               "  rewrite %.2f ms per load over %d replies\n  css rules on %d / off %d, text on %d / off %d\n",
               argv[k], on / 3, off / 3, us / 3000.0, replies, a.rules, b.rules, a.text, b.text);
        fflush(stdout);
    }
    return 0;
}
