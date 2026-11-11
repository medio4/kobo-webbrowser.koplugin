#include "webpage.h"
#include "memtune.h"
#include "adblock.h"
#include <QtWebKit/QWebSettings>
#include <QtWebKitWidgets/QWebFrame>
#include <QtNetwork/QNetworkRequest>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QTimer>
#include <QtCore/QStringList>
#include <QtCore/QDebug>
#include <cstdlib>

static const char *kMobileUA =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 9_3 like Mac OS X) AppleWebKit/601.1.46 (KHTML, like Gecko) "
    "Version/9.0 Mobile/13E233 Safari/601.1 KoboBrowser/0.1";

static const char *kEinkCss =
    "html,body{background:#fff !important;color:#000 !important}"
    "a,a:visited,a:link{color:#000 !important;text-decoration:underline !important}"
    "*{text-shadow:none !important;box-shadow:none !important;transition:none !important;animation:none !important}"
    "img,video{max-width:100% !important;height:auto}"
    "body{line-height:1.45}"
    // Wikipedia (Vector 2022 / Minerva): hide chrome that this WebKit can't lay out.
    ".vector-header-container,.vector-page-toolbar,.vector-column-start,.vector-column-end,"
    ".vector-sticky-header,#p-lang-btn,.vector-page-titlebar-toc,#vector-toc-pinned-container,"
    ".mw-footer-container,#mw-navigation,.header-container,.minerva-footer,.mw-editsection,"
    ".mw-indicators,.pre-content .page-actions-menu,.last-modified-bar{display:none !important}"
    ".mw-body,.mw-page-container,.mw-content-container,.vector-body{margin:0 !important;padding:0 8px !important;max-width:none !important}";

// Bing Images, which stands in for Google Images (see mobileVariant), fills each
// thumbnail's src from data-src with a lazy-load script, and opens results in a
// JavaScript viewer. Do both by hand so the grid works whatever this WebKit can run:
// fill in the thumbnails, point each tile straight at the full image, and add a
// plain "More images" link in place of the script-driven "See more" button.
static const char *kBingImagesJs =
    "(function(){"
    "var im=document.querySelectorAll('img[data-src]');"
    "for(var i=0;i<im.length;i++){var s=im[i].getAttribute('data-src'),c=im[i].getAttribute('src');"
    " if(s&&(!c||c.indexOf('data:')==0))im[i].setAttribute('src',s);}"
    "var a=document.querySelectorAll('a[m]'),n=0;"
    "for(var j=0;j<a.length;j++){try{var m=JSON.parse(a[j].getAttribute('m'));n++;"
    " if(m.murl){a[j].setAttribute('href',m.murl);a[j].removeAttribute('target');}}catch(e){}}"
    "if(!n||document.getElementById('kb-more'))return;"
    "var q=(location.search.match(/[?&]q=([^&]*)/)||[])[1];if(!q)return;"
    "var f=parseInt((location.search.match(/[?&]first=(\\d+)/)||[])[1]||'1',10);"
    "if(location.pathname.indexOf('/images/async')!=0)f=1;"
    "var l=document.createElement('a');l.id='kb-more';l.textContent='More images \\u203a';"
    "l.href='/images/async?q='+q+'&first='+(f+n)+'&count=35';"
    "l.setAttribute('style','display:block;clear:both;font-size:1.3em;padding:16px;text-align:center');"
    "document.body.appendChild(l);"
    "})()";

static bool isBing(const QUrl &url)
{
    QString host = url.host().toLower();
    return host == "bing.com" || host.endsWith(".bing.com");
}

static bool isGoogle(const QString &host)
{
    // google.com, www.google.co.uk, images.google.de ...
    QStringList parts = host.split('.');
    if (parts.size() < 2) return false;
    if (parts.first() == "www" || parts.first() == "images") parts.removeFirst();
    if (parts.size() < 2 || parts.first() != "google") return false;
    return parts.size() <= 3;
}

KoboWebPage::KoboWebPage(QObject *parent) : QWebPage(parent)
{
    connect(mainFrame(), SIGNAL(loadFinished(bool)), this, SLOT(fixupSite()));
    connect(mainFrame(), SIGNAL(loadStarted()), this, SLOT(adblockReset()));
    connect(mainFrame(), SIGNAL(loadFinished(bool)), this, SLOT(adblockReport()));
    setNetworkAccessManager(new KoboNetworkAccessManager(this));
    if (!getenv("KB_NO_EINK_CSS")) {
        QByteArray css(kEinkCss);
        settings()->setUserStyleSheetUrl(QUrl("data:text/css;charset=utf-8;base64," + css.toBase64()));
    }
}

QString KoboWebPage::userAgentForUrl(const QUrl &url) const
{
    const char *ua = getenv("KB_USER_AGENT");
    if (ua && *ua) return QString::fromLocal8Bit(ua);
    if (getenv("KB_DESKTOP_UA")) return QWebPage::userAgentForUrl(url);
    return QString::fromLatin1(kMobileUA);
}

QUrl KoboWebPage::mobileVariant(const QUrl &url)
{
    if (getenv("KB_NO_MOBILE_SITES")) return url;
    QString host = url.host().toLower();
    // (Wikipedia now serves its mobile layout on the main host based on the user agent,
    //  and redirects xx.m.wikipedia.org back, so no rewrite for it.)
    // Google Images now builds its results only with JavaScript this WebKit can't run,
    // and sends no-JS browsers an "update your browser" page, so send image searches
    // to Bing Images (fixed up in fixupSite).
    if (isGoogle(host)) {
        QUrlQuery query(url);
        QString path = url.path();
        bool images = host.startsWith("images.") || path.startsWith("/imghp")
            || query.queryItemValue("tbm") == "isch" || query.queryItemValue("udm") == "2";
        if (images) {
            QString q = query.queryItemValue("q", QUrl::FullyDecoded);
            QUrl u("https://www.bing.com/images/search");
            if (!q.isEmpty()) { QUrlQuery bq; bq.addQueryItem("q", q); u.setQuery(bq); }
            else u.setPath("/images/");
            return u;
        }
    }
    // A Bing Images tile that still points at Bing's JavaScript viewer: open the image.
    if (isBing(url) && url.path().startsWith("/images/search")) {
        QUrlQuery query(url);
        if (query.queryItemValue("view") == "detailV2") {
            QString media = query.queryItemValue("mediaurl", QUrl::FullyDecoded);
            if (media.isEmpty()) media = query.queryItemValue("cdnurl", QUrl::FullyDecoded);
            QUrl m(media);
            if (m.isValid() && (m.scheme() == "http" || m.scheme() == "https")) return m;
        }
    }
    if (host == "reddit.com" || host == "www.reddit.com") { QUrl u = url; u.setHost("old.reddit.com"); return u; }
    if (host == "duckduckgo.com" || host == "www.duckduckgo.com") {
        if (!url.path().startsWith("/lite")) { QUrl u = url; u.setHost("lite.duckduckgo.com"); u.setPath("/lite/"); return u; }
    }
    return url;
}

bool KoboWebPage::acceptNavigationRequest(QWebFrame *frame, const QNetworkRequest &request, NavigationType type)
{
    QUrl alt = mobileVariant(request.url());
    if (alt != request.url() && frame == mainFrame()) {
        qDebug() << "redirecting to mobile site" << alt.toString();
        mainFrame()->load(alt);
        return false;
    }
    bool ok = QWebPage::acceptNavigationRequest(frame, request, type);
    // A page the user asked for loads even if its host is on the blocklist;
    // only subresources and subframes are filtered.
    if (ok && (!frame || frame == mainFrame())) AdBlock::instance()->allowNavigation(request.url());
    return ok;
}

void KoboWebPage::adblockReset()
{
    AdBlock::instance()->resetCount();
}

void KoboWebPage::adblockReport()
{
    AdBlock *a = AdBlock::instance();
    if (a->isEnabled())
        qDebug() << "adblock:" << a->blockedSinceReset() << "requests blocked on" << mainFrame()->url().host();
}

void KoboWebPage::fixupSite()
{
    if (!isBing(mainFrame()->url())) return;
    runBingFixup();
    // Bing's own scripts may rebuild tiles shortly after load; go over them again.
    QTimer::singleShot(1500, this, SLOT(runBingFixup()));
}

void KoboWebPage::runBingFixup()
{
    if (isBing(mainFrame()->url())) mainFrame()->evaluateJavaScript(QString::fromLatin1(kBingImagesJs));
}
