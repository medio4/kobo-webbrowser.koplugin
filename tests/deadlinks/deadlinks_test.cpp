// Desktop test for src/deadlinks.h: links that carry their address in a data- attribute
// and rely on a site script to follow it. Run with tests/deadlinks/run.sh.
#include "../../src/deadlinks.h"
#include <QtWidgets/QApplication>
#include <QtWebKitWidgets/QWebPage>
#include <QtWebKitWidgets/QWebFrame>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void wait(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, SLOT(quit())); l.exec(); }

static const char *kPage =
    "<html><body>"
    "<a id='dead' href='javascript:void(0)' data-nav-url='/story/1'><b id='deadInner'>Story</b></a>"
    "<a id='hash' href='#' data-href='/story/2'>Hash</a>"
    "<a id='nohref' data-url='https://example.test/story/3'>No href</a>"
    "<a id='works' href='javascript:void(0)' data-nav-url='/story/4' "
    "onclick=\"document.body.className='busy'\">Handled by the site</a>"
    "<a id='prevented' href='javascript:void(0)' data-nav-url='/story/5' "
    "onclick='event.preventDefault()'>Prevented</a>"
    "<a id='notaurl' href='javascript:void(0)' data-url='modal-7'>Not an address</a>"
    "<a id='plain' href='javascript:void(0)'>Nothing to rescue</a>"
    "</body></html>";

// Clicks #id on a fresh page; returns the address the page went to ("" if it stayed).
static QString openedAfterClick(const char *id)
{
    QWebPage page;
    QWebFrame *f = page.mainFrame();
    f->setHtml(kPage, QUrl("http://example.test/"));
    f->evaluateJavaScript(kDeadLinksJs);
    f->evaluateJavaScript(kDeadLinksJs);   // twice: installs once
    f->evaluateJavaScript(QString("document.getElementById('%1').click()").arg(id));
    wait(600);
    QString got = f->url().toString() == "http://example.test/" ? QString() : f->url().toString();
    if (getenv("VERBOSE")) printf("%s -> \"%s\" at %s\n", id, qPrintable(got), qPrintable(f->url().toString()));
    return got;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    CHECK(openedAfterClick("dead") == "http://example.test/story/1");
    CHECK(openedAfterClick("deadInner") == "http://example.test/story/1");   // a tap on text inside the link
    CHECK(openedAfterClick("hash") == "http://example.test/story/2");
    CHECK(openedAfterClick("nohref") == "https://example.test/story/3");
    CHECK(openedAfterClick("works") == "");        // the site's script reacted
    CHECK(openedAfterClick("prevented") == "");    // the site's script took the click
    CHECK(openedAfterClick("notaurl") == "");
    CHECK(openedAfterClick("plain") == "");
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
