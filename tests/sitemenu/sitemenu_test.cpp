// Desktop test for src/sitemenu.h: the scripts that notice a site menu button that does
// nothing and collect the page's navigation links. Run with tests/sitemenu/run.sh.
#include "../../src/sitemenu.h"
#include <QtWidgets/QApplication>
#include <QtWebKitWidgets/QWebPage>
#include <QtWebKitWidgets/QWebFrame>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void wait(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, SLOT(quit())); l.exec(); }

static const char *kPage =
    "<html><body><header>"
    "<button id='dead' class='header__menu-icon' aria-label='Open Menu Icon'><span id='deadInner'>=</span></button>"
    "<button id='works' class='nav-toggle' onclick=\"document.body.className='open'\">=</button>"
    "<label id='css' class='menu-button' for='chk'>=</label><input type='checkbox' id='chk'>"
    "<a id='link' class='menu-link' href='/menu-page'>Menu</a>"
    "<button id='other' class='subscribe'>Subscribe</button>"
    "<a id='dead2' href='#' role='button' aria-expanded='false' aria-controls='site-nav'>=</a>"
    "<nav id='site-nav' style='display:none'>"
    "<a href='/us'> US </a><a href='/world'>World</a><a href='/world#top'>World again</a>"
    "<a href='javascript:void(0)'>Script</a><a href='/politics'><b>Politics</b></a><a href='/x'></a>"
    "</nav></header><p><a href='/story'>A story, not navigation</a></p></body></html>";

// Clicks #id and says whether the watcher marked it as a dead menu button.
static bool deadAfterClick(QWebFrame *f, const char *id)
{
    f->evaluateJavaScript(QString("document.getElementById('%1').click()").arg(id));
    wait(600);
    return f->evaluateJavaScript(kSiteMenuDeadJs).toInt() != 0;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QWebPage page;
    QWebFrame *f = page.mainFrame();
    f->setHtml(kPage, QUrl("http://example.test/"));
    f->evaluateJavaScript(kSiteMenuWatchJs);
    f->evaluateJavaScript(kSiteMenuWatchJs);   // twice: installs once

    CHECK(deadAfterClick(f, "dead"));
    CHECK(!f->evaluateJavaScript(kSiteMenuDeadJs).toInt());   // the mark is cleared when read
    CHECK(deadAfterClick(f, "deadInner"));    // a tap on the icon inside the button
    CHECK(deadAfterClick(f, "dead2"));        // aria-expanded + aria-controls
    CHECK(!deadAfterClick(f, "works"));       // the site's script opened its menu
    CHECK(!deadAfterClick(f, "css"));         // checkbox menus need no script
    CHECK(!deadAfterClick(f, "other"));       // not a menu button

    QString links = f->evaluateJavaScript(kSiteMenuLinksJs).toString();
    CHECK(links == "US\thttp://example.test/us\nWorld\thttp://example.test/world\nPolitics\thttp://example.test/politics");
    if (failures) printf("links:\n%s\n", qPrintable(links));

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
