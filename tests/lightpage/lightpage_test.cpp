// Desktop test for src/lightpage.h: a page that is mostly dark boxes is turned light, an
// ordinary page with a dark header is left alone. Run with tests/lightpage/run.sh.
#include "../../src/lightpage.h"
#include <QtWidgets/QApplication>
#include <QtWebKitWidgets/QWebPage>
#include <QtWebKitWidgets/QWebFrame>
#include <QtWebKit/QWebSettings>
#include <QtCore/QUrl>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const char *kCards =   // a light page background (as the browser forces) with dark cards on it
    "<div class='card' style='background:#32383e;color:#eee;height:400px'><a id='l' style='color:#5bc0de'>story</a></div>"
    "<div class='card' style='background:#272b30;color:#eee;height:500px'>text</div>";
static const char *kNews =    // a black header bar on an otherwise white page
    "<div style='background:#000;color:#fff;height:90px'>SITE</div>"
    "<div style='background:#fff;height:900px'>article</div><div style='background:#f4f4f4;height:600px'>more</div>";

struct Result { int turned; QString cls, bg, fg, link; };
static Result run(const char *body)
{
    QWebPage page;
    page.setViewportSize(QSize(536, 724));
    QByteArray css = QByteArray("html,body{background:#fff !important;color:#000 !important}") + kLightPageCss;
    page.settings()->setUserStyleSheetUrl(QUrl("data:text/css;charset=utf-8;base64," + css.toBase64()));
    QWebFrame *f = page.mainFrame();
    f->setHtml(QString("<html><body>%1</body></html>").arg(body), QUrl("http://example.test/"));
    Result r;
    r.turned = f->evaluateJavaScript(kLightPageJs).toInt();
    r.cls = f->evaluateJavaScript("document.documentElement.className").toString().trimmed();
    r.bg = f->evaluateJavaScript("getComputedStyle(document.body.children[0]).backgroundColor").toString();
    r.fg = f->evaluateJavaScript("getComputedStyle(document.body.children[0]).color").toString();
    r.link = f->evaluateJavaScript("var l=document.getElementById('l');l?getComputedStyle(l).color:''").toString();
    return r;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    Result c = run(kCards);
    CHECK(c.turned == 1 && c.cls == "kb-light");
    CHECK(c.bg == "rgb(255, 255, 255)" && c.fg == "rgb(0, 0, 0)" && c.link == "rgb(0, 0, 0)");
    Result n = run(kNews);
    CHECK(n.turned == 0 && n.cls.isEmpty());
    CHECK(n.bg == "rgb(0, 0, 0)" && n.fg == "rgb(255, 255, 255)");   // the header keeps its colours
    if (failures) printf("cards: %d '%s' %s %s %s\nnews: %d '%s' %s %s\n", c.turned, qPrintable(c.cls), qPrintable(c.bg),
                         qPrintable(c.fg), qPrintable(c.link), n.turned, qPrintable(n.cls), qPrintable(n.bg), qPrintable(n.fg));
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
