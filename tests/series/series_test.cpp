// Desktop test for src/js/series.js: which pages are taken for a list of parts, and which
// links are the parts. Run with tests/series/run.sh.
#include <QtWidgets/QApplication>
#include <QtWebKitWidgets/QWebPage>
#include <QtWebKitWidgets/QWebFrame>
#include <QtCore/QFile>
#include <QtCore/QUrl>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static QString g_js;

// "n: url1 url2 ..." (paths only) for the page at `url` with this body.
static QString run(const char *url, const char *body)
{
    QWebPage page;
    QWebFrame *f = page.mainFrame();
    f->setHtml(QString("<html><head><title>A Story</title></head><body>%1</body></html>").arg(body), QUrl(url));
    f->evaluateJavaScript("window.__s=" + g_js);
    return f->evaluateJavaScript(
        "(function(){var c=JSON.parse(window.__s).chapters;return c.length+':'+c.map(function(x){"
        "return ' '+x.url.replace(/^https?:\\/\\/[^\\/]+/,'')}).join('')})()").toString();
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QFile js("/src/src/js/series.js");
    if (!js.open(QIODevice::ReadOnly)) { printf("FAIL cannot read series.js\n"); return 1; }
    g_js = QString::fromUtf8(js.readAll());

    // Numbered addresses next to this page.
    CHECK(run("http://example.test/guide/part-1.html",
              "<a href='part-2.html'>Next</a> <a href='part-3.html'>Last</a> <a href='/about'>About</a>")
          == "3: /guide/part-1.html /guide/part-2.html /guide/part-3.html");

    // A list page whose parts live in another folder under long ids, told apart only by
    // "1: ...", "2: ..." in front of their titles. A comments link to the same part and a
    // link elsewhere with a number in it don't count.
    const char *list =
        "<a href='/user/show/77'>An Author</a>"
        "<div><a href='/story/show/4000123'> 1: The Start </a><a href='/story/show/4000123#comments'>15</a></div>"
        "<div><a href='/story/show/4000456'>2: Next Day</a></div>"
        "<div><a href='/story/show/4000789'>3: The Rescue</a></div>"
        "<a href='/series/show/3000042?linear=true'>All chapters</a>";
    CHECK(run("http://example.test/series/show/3000042", list)
          == "3: /story/show/4000123 /story/show/4000456 /story/show/4000789");
    CHECK(run("http://example.test/book/55",
              "<a href='/read/901'>Chapter 1 - In</a><a href='/read/907'>Chapter 2 - Out</a>")
          == "2: /read/901 /read/907");

    // Not a list of parts: a gap in the numbers, one numbered link, or numbers that are
    // part of ordinary titles.
    CHECK(run("http://example.test/series/show/3000042",
              "<a href='/story/show/1'>1: One</a><a href='/story/show/3'>3: Three</a>") == "0:");
    CHECK(run("http://example.test/series/show/3000042", "<a href='/story/show/1'>1: One</a>") == "0:");
    CHECK(run("http://example.test/wiki/Moon",
              "<a href='/wiki/Apollo_11'>Apollo 11</a><a href='/wiki/Apollo_12'>Apollo 12</a>") == "0:");

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
